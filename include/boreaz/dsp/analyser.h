#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "boreaz/dsp/tuning.h"

namespace boreaz::dsp {

// One slot of the master output handed from the audio thread to the frame loop.
// The master output copied to whoever is looking: a fixed ring, one writer and one reader, that
// overwrites itself rather than waiting.
//
// The audio thread cannot stall on a display. A ring that blocks when the frame loop is slow would put
// the window's frame rate inside the audio path's timing, which is the one rule this program is built
// around (plan 3). So the writer never waits: when the reader has not kept up, the oldest frames are
// thrown away and the count of what was thrown is kept, so the panel can say how much of the sound it
// missed instead of pretending it saw all of it.
//
// Indices are monotonic frame numbers, masked on use, so a wrap is arithmetic and not a branch, and the
// producer and consumer each touch exactly one atomic.
class SampleTap {
public:
    // Frames of stereo held. A power of two so the index is a mask, and wide enough that a 60 fps frame
    // loop drains it with room: a 16 ms frame is 768 frames at 48 kHz, and this is 8192.
    static constexpr int kCapacityFrames = 8192;

    SampleTap() = default;

    // The audio thread. Never waits, never allocates, never reads what the reader reads.
    void write(const float* interleaved, int frames);

    // The frame loop: the frames it can still get, oldest first, in the same left/right pairs the master
    // bus uses. Returns how many frames were copied.
    int read(float* interleaved, int max_frames);

    // What was thrown away because nobody was draining, and how much has been offered at all. The panel
    // prints the first when it is not zero.
    std::uint64_t dropped_frames() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t written_frames() const { return written_.load(std::memory_order_relaxed); }

    // A frame loop that stops for a while: how far behind the writer is right now.
    int pending_frames() const {
        const std::uint64_t behind = written_.load(std::memory_order_relaxed) -
                                     read_.load(std::memory_order_relaxed);
        return static_cast<int>(std::min<std::uint64_t>(behind, static_cast<std::uint64_t>(kCapacityFrames)));
    }

    void reset() {
        written_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr std::uint64_t kMask = static_cast<std::uint64_t>(kCapacityFrames) - 1;

    std::array<float, static_cast<std::size_t>(kCapacityFrames) * 2> data_{};
    std::atomic<std::uint64_t> written_{0}; // the writer's own cursor
    std::atomic<std::uint64_t> read_{0};    // the reader's own cursor
    std::atomic<std::uint64_t> dropped_{0};
};

// A log-spaced analyser: samples in, dB per semitone band out, with a peak that falls.
//
// 88 bands, one per key of the instrument (A0 to C8), each covering the half-semitone either side of
// its note. The x placement is not here: it is `KeyboardLayout`, which the spectrum panel asks for the
// same width the keyboard below is drawn at, so a band sits over the key that plays it and the spectrum
// is the waterfall wearing a colour ramp instead of a scroll (plan 1).
//
// One window cannot do both jobs, so there are two sizes and the panel picks one: 8192 samples where
// the bins have to be narrow (~5.9 Hz at 48 kHz, and a semitone below C3 is narrower than that), and
// 2048 where a transient has to light now (~43 ms, about a semitone at C8).
//
// Header-only, like everything in `boreaz_dsp`: the library is an INTERFACE target that links nothing,
// which is what keeps an FFT from growing an I/O dependency by accident.
class Analyser {
public:
    static constexpr int kMaxWindow = 8192;
    static constexpr int kMinWindow = 256; // radix-2 only; small sizes are what the tests run on
    static constexpr int kWideWindow = 8192;
    static constexpr int kFastWindow = 2048;
    static constexpr int kBandCount = Tuning::kLastKey - Tuning::kFirstKey + 1; // 88
    static constexpr float kFloorDb = -120.0f;
    static constexpr float kDefaultPeakDecayDbPerSecond = 14.0f;

    Analyser() = default;
    Analyser(int window_samples, double sample_rate);

    // A size that is not a power of two is rounded down to the previous one, and both ends are clamped,
    // so a config file cannot ask for a transform that does not exist. Returns the size actually used.
    int configure(int window_samples, double sample_rate);

    void set_sample_rate(double sample_rate) { configure(window_size_, sample_rate); }
    int window_size() const { return window_size_; }
    double sample_rate() const { return sample_rate_; }
    static constexpr int band_count() { return kBandCount; }

    // The master is stereo; the analyser looks at the sum, because "what the speakers get" is one sound
    // and a band that only lights when the note is panned left is a bug a player cannot tell apart from
    // a note that was not played.
    void push(const float* interleaved_stereo, int frames);

    // True when a full window of new samples has arrived since the last transform, and this one ran.
    bool transform();

    // Let the peaks fall. Once per frame, with the frame loop's own clock, so the decay is in the unit
    // the knob says it is in.
    void advance(double now_seconds);

    // Forget the past: the bands go back to the floor and the sound held between transforms is
    // dropped. A hidden panel calls this. Left alone, it keeps showing the last picture it was
    // handed, and a meter frozen at the level of a minute ago reads as "nothing is playing now" -
    // the one sentence that is not true. The count of frames that were offered and never analysed is
    // deliberately not cleared: it is evidence, and it belongs to the whole session.
    void reset() {
        band_db_.fill(kFloorDb);
        peak_db_.fill(kFloorDb);
        pushed_ = 0;
        consumed_ = 0;
        last_advance_seconds_ = -1.0;
    }

    void set_peak_decay_db_per_second(float db_per_second) {
        peak_decay_ = std::clamp(db_per_second, 0.0f, 120.0f);
    }
    float peak_decay_db_per_second() const { return peak_decay_; }

    float band_db(int index) const {
        return band_db_[static_cast<std::size_t>(index)]; // the panel never asks out of range
    }
    float peak_db(int index) const { return peak_db_[static_cast<std::size_t>(index)]; }
    float peak_db_at_note(int midi_note) const;
    float band_db_at_note(int midi_note) const; // the band a note plays into, or the floor

    std::uint64_t transform_count() const { return transforms_; }
    double last_microseconds() const { return last_microseconds_; } // the panel prints this
    std::uint64_t dropped_input_frames() const { return dropped_input_; }

    // The band a frequency belongs in, and the frequency of a band's note. Used by the panel's labels
    // and by the tests that name the note they are aiming at.
    static int band_for_frequency(double hz);
    static double band_hz(int index);

private:
    void build_window();
    void build_bands();
    void run_transform();
    float peak_amplitude(int lo, int hi) const;

    int window_size_ = 0;
    int window_shift_ = 0; // log2 of the size, so the FFT's butterflies are shifts and not divisions
    double sample_rate_ = 0.0;
    float peak_decay_ = kDefaultPeakDecayDbPerSecond;

    std::array<float, static_cast<std::size_t>(kMaxWindow)> hann_{};
    std::array<float, static_cast<std::size_t>(kMaxWindow)> re_{};
    std::array<float, static_cast<std::size_t>(kMaxWindow)> im_{};

    // The newest samples, in a ring the size of the biggest window, so both analysers can be fed the
    // same stream without either of them keeping a copy of the whole master bus.
    std::array<float, static_cast<std::size_t>(kMaxWindow)> history_{};
    std::uint64_t pushed_ = 0;    // frames ever pushed
    std::uint64_t consumed_ = 0;  // frames covered by the last transform
    std::uint64_t dropped_input_ = 0;

    // bin range of each band, half-open
    std::array<int, static_cast<std::size_t>(kBandCount)> band_low_{};
    std::array<int, static_cast<std::size_t>(kBandCount)> band_high_{};
    std::array<float, static_cast<std::size_t>(kBandCount)> band_db_{};
    std::array<float, static_cast<std::size_t>(kBandCount)> peak_db_{};

    std::uint64_t transforms_ = 0;
    double last_microseconds_ = 0.0;
    double last_advance_seconds_ = -1.0;
};

// The definitions below stay inside this namespace: an out-of-class definition written outside it names
// a class that does not exist where it is written.

// ---------------------------------------------------------------- the tap

inline void SampleTap::write(const float* interleaved, int frames) {
    if (interleaved == nullptr || frames <= 0) {
        return;
    }
    // shortcut: a frame loop that stalls longer than the ring holds (170 ms at 48 kHz) can be overtaken
    // mid-copy and see one frame of a mixture. Upgrades when the tap feeds something that must not tear.
    const std::uint64_t start = written_.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        // The mask is in frames, and a frame is two floats: masking the float index instead would put
        // a right channel where a left channel belongs whenever the ring wrapped.
        const std::uint64_t slot = (start + static_cast<std::uint64_t>(i)) & kMask;
        data_[static_cast<std::size_t>(slot * 2)] = interleaved[static_cast<std::size_t>(i * 2)];
        data_[static_cast<std::size_t>(slot * 2 + 1)] = interleaved[static_cast<std::size_t>(i * 2 + 1)];
    }
    // Release so a reader that sees the new cursor also sees the samples behind it.
    written_.store(start + static_cast<std::uint64_t>(frames), std::memory_order_release);
}

inline int SampleTap::read(float* dest, int max_frames) {
    if (dest == nullptr || max_frames <= 0) {
        return 0;
    }
    const std::uint64_t available = written_.load(std::memory_order_acquire);
    std::uint64_t here = read_.load(std::memory_order_relaxed);
    // The reader, not the writer, moves the reader's cursor: each side owns one atomic, which is what
    // keeps this a single-producer / single-consumer ring instead of something that needs a lock. The
    // frames it jumped over are counted, so the panel can print the size of the hole it has in it.
    if (available > here + static_cast<std::uint64_t>(kCapacityFrames)) {
        const std::uint64_t jumped = available - static_cast<std::uint64_t>(kCapacityFrames) - here;
        dropped_.store(dropped_.load(std::memory_order_relaxed) + jumped, std::memory_order_relaxed);
        here = available - static_cast<std::uint64_t>(kCapacityFrames);
    }
    std::uint64_t take = available - here;
    if (take > static_cast<std::uint64_t>(max_frames)) {
        take = static_cast<std::uint64_t>(max_frames);
    }
    for (std::uint64_t i = 0; i < take; ++i) {
        const std::uint64_t slot = (here + i) & kMask;
        dest[static_cast<std::size_t>(i * 2)] = data_[static_cast<std::size_t>(slot * 2)];
        dest[static_cast<std::size_t>(i * 2 + 1)] = data_[static_cast<std::size_t>(slot * 2 + 1)];
    }
    read_.store(here + take, std::memory_order_relaxed);
    return static_cast<int>(take);
}

// ---------------------------------------------------------------- the analyser

inline Analyser::Analyser(int window_samples, double sample_rate) {
    configure(window_samples, sample_rate);
}

inline int Analyser::configure(int window_samples, double sample_rate) {
    int size = std::max(window_samples, kMinWindow);
    if (size > kMaxWindow) {
        size = kMaxWindow;
    }
    // Round down to a power of two: radix-2 is the only transform here, and asking for 6000 bins is a
    // request for the 4096 that exist, not an error the display should die on.
    int shift = 0;
    while ((1 << (shift + 1)) <= size) {
        ++shift;
    }
    size = 1 << shift;

    window_size_ = size;
    window_shift_ = shift;
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 0.0;
    pushed_ = 0;
    consumed_ = 0;
    dropped_input_ = 0;
    transforms_ = 0;
    last_advance_seconds_ = -1.0;
    band_db_.fill(kFloorDb);
    peak_db_.fill(kFloorDb);
    build_window();
    build_bands();
    return window_size_;
}

// The newest samples, summed to mono: the master is stereo, and a band that only lights when the note is
// panned left is a bug a player cannot tell apart from a note that was never played.
inline void Analyser::push(const float* interleaved_stereo, int frames) {
    if (interleaved_stereo == nullptr || frames <= 0) {
        return;
    }
    constexpr std::uint64_t history_mask = static_cast<std::uint64_t>(kMaxWindow) - 1;
    for (int i = 0; i < frames; ++i) {
        const float left = interleaved_stereo[static_cast<std::size_t>(i * 2)];
        const float right = interleaved_stereo[static_cast<std::size_t>(i * 2 + 1)];
        history_[(pushed_ + static_cast<std::uint64_t>(i)) & history_mask] = (left + right) * 0.5f;
    }
    pushed_ += static_cast<std::uint64_t>(frames);
}

// Periodic Hann, not symmetric: this window feeds a transform that assumes the sample after the last one
// is the sample before the first, and a symmetric Hann breaks that assumption at the ends.
inline void Analyser::build_window() {
    const double n = static_cast<double>(window_size_);
    for (int i = 0; i < window_size_; ++i) {
        hann_[static_cast<std::size_t>(i)] =
            static_cast<float>(0.5 * (1.0 - std::cos(2.0 * 3.14159265358979323846 * static_cast<double>(i) / n)));
    }
}

inline void Analyser::build_bands() {
    if (sample_rate_ <= 0.0) {
        return;
    }
    const double bin_hz = sample_rate_ / static_cast<double>(window_size_);
    const int last_bin = window_size_ / 2;
    for (int band = 0; band < kBandCount; ++band) {
        const double hz = band_hz(band);
        // Half a semitone either side, so every bin belongs to the note it is closest to and the bands
        // tile the spectrum without a hole between two keys.
        int lo = static_cast<int>(std::floor((hz * std::pow(2.0, -1.0 / 24.0)) / bin_hz));
        int hi = static_cast<int>(std::ceil((hz * std::pow(2.0, 1.0 / 24.0)) / bin_hz));
        // Bin 0 is DC and is never part of a band: a steady offset is not a note, and the plan says the
        // lowest band must prove it knows the difference.
        lo = std::max(lo, 1);
        hi = std::max(std::min(hi, last_bin), lo + 1);
        band_low_[static_cast<std::size_t>(band)] = lo;
        band_high_[static_cast<std::size_t>(band)] = hi;
    }
}

// The transform itself: radix-2 Cooley-Tukey, iterative, on the newest window.
inline void Analyser::run_transform() {
    const int n = window_size_;
    const std::uint64_t end = pushed_;
    constexpr std::uint64_t history_mask = static_cast<std::uint64_t>(kMaxWindow) - 1;
    for (int i = 0; i < n; ++i) {
        re_[static_cast<std::size_t>(i)] =
            history_[(end - static_cast<std::uint64_t>(n) + static_cast<std::uint64_t>(i)) & history_mask];
        im_[static_cast<std::size_t>(i)] = 0.0f;
    }

    // DC out before the window: a steady offset would come through the window's skirt and light the
    // lowest band, and the plan asks specifically that a steady offset not be mistaken for a low note.
    double total = 0.0;
    for (int i = 0; i < n; ++i) {
        total += static_cast<double>(re_[static_cast<std::size_t>(i)]);
    }
    const float mean = static_cast<float>(total / static_cast<double>(n));
    for (int i = 0; i < n; ++i) {
        re_[static_cast<std::size_t>(i)] =
            (re_[static_cast<std::size_t>(i)] - mean) * hann_[static_cast<std::size_t>(i)];
    }

    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re_[static_cast<std::size_t>(i)], re_[static_cast<std::size_t>(j)]);
            std::swap(im_[static_cast<std::size_t>(i)], im_[static_cast<std::size_t>(j)]);
        }
    }

    constexpr double kPi = 3.14159265358979323846;
    for (int len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const float wr = static_cast<float>(std::cos(angle));
        const float wi = static_cast<float>(std::sin(angle));
        const int half = len >> 1;
        for (int start = 0; start < n; start += len) {
            float cr = 1.0f;
            float ci = 0.0f;
            for (int k = 0; k < half; ++k) {
                const int a = start + k;
                const int b = a + half;
                const float vr = re_[static_cast<std::size_t>(b)] * cr - im_[static_cast<std::size_t>(b)] * ci;
                const float vi = re_[static_cast<std::size_t>(b)] * ci + im_[static_cast<std::size_t>(b)] * cr;
                re_[static_cast<std::size_t>(b)] = re_[static_cast<std::size_t>(a)] - vr;
                im_[static_cast<std::size_t>(b)] = im_[static_cast<std::size_t>(a)] - vi;
                re_[static_cast<std::size_t>(a)] += vr;
                im_[static_cast<std::size_t>(a)] += vi;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }

    // One level per band: the loudest tone inside it, at the level that tone really has.
    //
    // The first attempt summed the band's bins in quadrature, and for a lone tone that is wrong by a
    // constant +1.76 dB: a periodic Hann leaves the bins either side of a peak at a quarter of its power,
    // and quadrature adds those to 1.5 times the peak. Taking the tallest bin alone is wrong the other way,
    // by up to -1.42 dB, because a tone almost never lands on a bin and the window splits its height.
    // Neither error is noise, and a spectrum that is a fixed 1.76 dB confident about nothing is worse than
    // one that says nothing.
    for (int band = 0; band < kBandCount; ++band) {
        const float amplitude = peak_amplitude(band_low_[static_cast<std::size_t>(band)],
                                               band_high_[static_cast<std::size_t>(band)]);
        band_db_[static_cast<std::size_t>(band)] =
            amplitude > 1.0e-9f ? static_cast<float>(20.0 * std::log10(amplitude)) : kFloorDb;
        // The peak snaps up the instant the note is there and only ever falls between transforms, so the
        // hold line is the level of the loudest recent note rather than a trace of what the panel happened
        // to be drawing.
        peak_db_[static_cast<std::size_t>(band)] =
            std::max(peak_db_[static_cast<std::size_t>(band)], band_db_[static_cast<std::size_t>(band)]);
    }
}

// The height of a tone from three samples of the window's main lobe. A periodic Hann's lobe is a rational
// function of the bin offset with three independent samples in it, so the offset and the height can be
// solved rather than guessed: with a, b, c the magnitudes at k-1, k, k+1, the offset off bin k is
// d = (c - a) / (a + b + c) and the lobe's own height is b / (1 - d^2). Measured over every offset from a
// bin to half a bin away, that reads within 0.2 dB of the level the generator used; the tallest bin alone
// reads 1.4 dB low at the halfway point.
inline float Analyser::peak_amplitude(int lo, int hi) const {
    const int n = window_size_;
    // A periodic Hann sums to exactly N/2, and a sine of amplitude A on a bin measures A*N/4, so scaling
    // by 4/N puts the reading in amplitude relative to full scale: -20 dBFS is 0.1.
    const double scale = 4.0 / static_cast<double>(n);
    const int first = std::max(lo, 1);            // bin 0 is DC and belongs to no band
    const int last = std::min(hi - 1, n / 2 - 1); // and the mirror above Nyquist is the same tone
    double best = 0.0;
    for (int bin = first; bin <= last; ++bin) {
        const double centre = std::hypot(re_[static_cast<std::size_t>(bin)],
                                         im_[static_cast<std::size_t>(bin)]);
        const double before = std::hypot(re_[static_cast<std::size_t>(bin - 1)],
                                         im_[static_cast<std::size_t>(bin - 1)]);
        const double after = std::hypot(re_[static_cast<std::size_t>(bin + 1)],
                                        im_[static_cast<std::size_t>(bin + 1)]);
        const double total = before + centre + after;
        if (total <= 0.0) {
            continue;
        }
        const double offset = (after - before) / total;
        const double height = centre / (1.0 - offset * offset);
        best = std::max(best, height * scale);
    }
    return static_cast<float>(best);
}

inline bool Analyser::transform() {
    if (window_size_ == 0 || sample_rate_ <= 0.0 || pushed_ < static_cast<std::uint64_t>(window_size_)) {
        return false;
    }
    if (pushed_ - consumed_ < static_cast<std::uint64_t>(window_size_)) {
        return false; // not a window's worth of new sound since the last one
    }
    // Whatever is older than the newest window was never looked at. That is a hole in what the panel is
    // showing, so it is counted rather than smoothed over.
    if (pushed_ - consumed_ > static_cast<std::uint64_t>(window_size_)) {
        dropped_input_ += pushed_ - consumed_ - static_cast<std::uint64_t>(window_size_);
    }
    const auto start = std::chrono::steady_clock::now();
    run_transform();
    last_microseconds_ =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    consumed_ = pushed_;
    ++transforms_;
    return true;
}

inline void Analyser::advance(double now_seconds) {
    if (last_advance_seconds_ < 0.0) {
        last_advance_seconds_ = now_seconds;
        return;
    }
    const double elapsed = now_seconds - last_advance_seconds_;
    last_advance_seconds_ = now_seconds;
    if (elapsed <= 0.0) {
        return;
    }
    const float fall = peak_decay_ * static_cast<float>(elapsed);
    for (int band = 0; band < kBandCount; ++band) {
        const float held = peak_db_[static_cast<std::size_t>(band)] - fall;
        peak_db_[static_cast<std::size_t>(band)] = std::max(band_db_[static_cast<std::size_t>(band)],
                                                            std::max(held, kFloorDb));
    }
}

inline int Analyser::band_for_frequency(double hz) {
    if (!(hz > 0.0)) {
        return -1;
    }
    const double note = 69.0 + 12.0 * std::log2(hz / 440.0);
    int band = static_cast<int>(std::lround(note)) - Tuning::kFirstKey;
    return std::clamp(band, 0, kBandCount - 1);
}

// Bands are anchored to A4 = 440 Hz and 12-TET semitone widths, not to the app's reference pitch: the x
// axis of a spectrum is the audio being measured, which does not move because the instrument was tuned
// to 432. A sine at 440 Hz belongs to A4's band whichever tuning is loaded.
inline double Analyser::band_hz(int index) {
    const int note = Tuning::kFirstKey + std::clamp(index, 0, kBandCount - 1);
    return 440.0 * std::pow(2.0, static_cast<double>(note - 69) / 12.0);
}

inline float Analyser::band_db_at_note(int midi_note) const {
    if (midi_note < Tuning::kFirstKey || midi_note > Tuning::kLastKey) {
        return kFloorDb;
    }
    return band_db_[static_cast<std::size_t>(midi_note - Tuning::kFirstKey)];
}

inline float Analyser::peak_db_at_note(int midi_note) const {
    if (midi_note < Tuning::kFirstKey || midi_note > Tuning::kLastKey) {
        return kFloorDb;
    }
    return peak_db_[static_cast<std::size_t>(midi_note - Tuning::kFirstKey)];
}

} // namespace boreaz::dsp
