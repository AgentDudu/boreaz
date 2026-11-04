// The analyser and the tap it is fed from: bands that have to land on the right note, at the right
// level, from a ring that may never wait for the display.
//
// Synthetic buffers, on purpose. The plan's criteria for this panel are statements about known
// frequencies and known levels, and a sine generated here is the only way to know both. What is not
// here is what only a live window knows - whether the frame loop kept up, and what the analyser costs
// per frame - and docs/ reports those separately.

#include "boreaz/dsp/analyser.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

using boreaz::dsp::Analyser;
using boreaz::dsp::SampleTap;

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;

// A4 = 440, 12-TET: the same anchoring the analyser says its bands use.
double note_hz(int midi_note) {
    return 440.0 * std::pow(2.0, static_cast<double>(midi_note - 69) / 12.0);
}

// One window of a sine, written to both channels, then one transform. `hz` is deliberately not an
// integer number of cycles per window, because a real note never is and the window has to cope anyway.
void push_one_window_of_sine(Analyser& analyser, double hz, double amplitude) {
    const int n = analyser.window_size();
    std::vector<float> block(static_cast<std::size_t>(n) * 2, 0.0f);
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const float v = static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * t));
        block[static_cast<std::size_t>(i * 2)] = v;
        block[static_cast<std::size_t>(i * 2 + 1)] = v;
    }
    analyser.push(block.data(), n);
}

int peak_band(const Analyser& analyser) {
    int best = 0;
    for (int band = 1; band < Analyser::band_count(); ++band) {
        if (analyser.band_db(band) > analyser.band_db(best)) {
            best = band;
        }
    }
    return best;
}

// The loudest reading in the three bands a note could plausibly land in: a filter bank that shares
// bins at the bottom of the range spreads a low note over its neighbours, and the plan's criterion is
// written as "the correct band and within one band of it" for exactly that reason.
float level_at_note(const Analyser& analyser, int midi_note) {
    float best = Analyser::kFloorDb;
    for (int note = midi_note - 1; note <= midi_note + 1; ++note) {
        best = std::max(best, analyser.band_db_at_note(note));
    }
    return best;
}

} // namespace

TEST_CASE("a lone sine lands in the band of the note it is") {
    Analyser analyser(Analyser::kWideWindow, kRate);
    // A0, C2, A2, C4, A4, C6, A6, C8: the bottom, the middle, and both octaves where a semitone is
    // narrower than a bin at any window length.
    for (int note : {21, 36, 45, 60, 69, 84, 93, 108}) {
        analyser.configure(Analyser::kWideWindow, kRate);
        push_one_window_of_sine(analyser, note_hz(note), 0.1); // -20 dBFS
        REQUIRE(analyser.transform());

        const int expected = note - boreaz::dsp::Tuning::kFirstKey;
        const int got = peak_band(analyser);
        INFO("note " << note << " at " << note_hz(note) << " Hz, band " << got << " reads "
                     << analyser.band_db(got) << " dB");
        CHECK(std::abs(got - expected) <= 1);
        CHECK(std::abs(level_at_note(analyser, note) - -20.0f) <= 1.5f);
    }
}

TEST_CASE("silence reads as silence") {
    Analyser analyser(Analyser::kWideWindow, kRate);
    std::vector<float> block(static_cast<std::size_t>(Analyser::kWideWindow) * 2, 0.0f);
    analyser.push(block.data(), Analyser::kWideWindow);
    REQUIRE(analyser.transform());
    for (int band = 0; band < Analyser::band_count(); ++band) {
        CHECK(analyser.band_db(band) < -100.0f);
    }
}

TEST_CASE("a steady offset is not a low note") {
    Analyser analyser(Analyser::kWideWindow, kRate);
    std::vector<float> block(static_cast<std::size_t>(Analyser::kWideWindow) * 2, 0.5f);
    analyser.push(block.data(), Analyser::kWideWindow);
    REQUIRE(analyser.transform());
    // DC would sit at bin 0, two bins below the lowest band, and a window's skirt is wide enough to
    // reach that far if the mean were left in.
    CHECK(analyser.band_db(0) < -80.0f);
    CHECK(peak_band(analyser) >= 0);
    CHECK(analyser.band_db(peak_band(analyser)) < -80.0f);
}

TEST_CASE("two notes a fifth apart leave a valley between them") {
    Analyser analyser(Analyser::kWideWindow, kRate);
    const double low = note_hz(60);         // C4
    const double fifth = note_hz(67);       // G4
    const int n = analyser.window_size();
    std::vector<float> block(static_cast<std::size_t>(n) * 2, 0.0f);
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const float v = static_cast<float>(
            0.1 * (std::sin(2.0 * kPi * low * t) + std::sin(2.0 * kPi * fifth * t)));
        block[static_cast<std::size_t>(i * 2)] = v;
        block[static_cast<std::size_t>(i * 2 + 1)] = v;
    }
    analyser.push(block.data(), n);
    REQUIRE(analyser.transform());

    const float c4 = analyser.band_db_at_note(60);
    const float g4 = analyser.band_db_at_note(67);
    INFO("C4 " << c4 << " dB, G4 " << g4 << " dB");
    CHECK(std::abs(c4 - g4) < 1.0f); // same amplitude, so both peaks read the same

    float valley = Analyser::kFloorDb;
    for (int note = 62; note <= 65; ++note) { // the bands between them
        valley = std::max(valley, analyser.band_db_at_note(note));
    }
    INFO("valley " << valley << " dB");
    CHECK(valley <= std::min(c4, g4) - 6.0f);
}

TEST_CASE("a window that is not a power of two becomes the one that is") {
    Analyser analyser;
    CHECK(analyser.configure(6000, kRate) == 4096);
    CHECK(analyser.configure(2048, kRate) == 2048);
    CHECK(analyser.configure(9000, kRate) == Analyser::kMaxWindow);
    CHECK(analyser.configure(100, kRate) == Analyser::kMinWindow);
    CHECK(analyser.configure(8192, kRate) == 8192);
    // A size asked for in the middle of nowhere is still a transform that exists, and the rate it was
    // given is the rate the bins are cut at.
    CHECK(analyser.sample_rate() == kRate);
    analyser.set_sample_rate(44100.0);
    CHECK(analyser.sample_rate() == 44100.0);
}

TEST_CASE("a twenty millisecond burst in the top octave lights its band, and only the short window is fast enough to see it") {
    const double hz = note_hz(108); // C8
    const int burst_frames = static_cast<int>(0.020 * kRate);

    Analyser fast(Analyser::kFastWindow, kRate);
    std::vector<float> block(static_cast<std::size_t>(Analyser::kFastWindow) * 2, 0.0f);
    for (int i = 0; i < burst_frames; ++i) {
        const float v = static_cast<float>(0.1 * std::sin(2.0 * kPi * hz * (static_cast<double>(i) / kRate)));
        block[static_cast<std::size_t>(i * 2)] = v;
        block[static_cast<std::size_t>(i * 2 + 1)] = v;
    }
    fast.push(block.data(), burst_frames);
    // One window's worth of silence after the burst: within two transforms the note has to be visible,
    // which is what "a transient has to light now" means on a screen.
    fast.push(block.data(), Analyser::kFastWindow - burst_frames);
    REQUIRE(fast.transform());
    INFO("C8 reads " << fast.band_db_at_note(108) << " dB after one window");
    CHECK(fast.band_db_at_note(108) > -30.0f);
    CHECK(peak_band(fast) >= Analyser::band_count() - 2);

    Analyser wide(Analyser::kWideWindow, kRate);
    wide.push(block.data(), Analyser::kFastWindow);
    CHECK_FALSE(wide.transform()); // 2048 samples is not a window's worth at 8192: the long window has not finished listening
    CHECK(wide.transform_count() == 0);
}

TEST_CASE("the peak holds, then falls at the rate the knob says") {
    Analyser analyser(Analyser::kFastWindow, kRate);
    analyser.set_peak_decay_db_per_second(20.0f);
    CHECK(analyser.peak_decay_db_per_second() == 20.0f);
    analyser.set_peak_decay_db_per_second(-1.0f); // out of range clamps instead of drawing a rising peak
    CHECK(analyser.peak_decay_db_per_second() == 0.0f);
    analyser.set_peak_decay_db_per_second(20.0f);

    push_one_window_of_sine(analyser, note_hz(69), 0.1);
    REQUIRE(analyser.transform());
    analyser.advance(0.0);
    const float played = analyser.band_db_at_note(69);
    CHECK(analyser.peak_db_at_note(69) == played); // a peak snaps up the instant the note is there

    // The hold line can only fall once the note has actually gone: while the band still reads the tone,
    // the peak is the tone. So the decay is measured after a window of silence.
    std::vector<float> quiet(static_cast<std::size_t>(Analyser::kFastWindow) * 2, 0.0f);
    analyser.push(quiet.data(), Analyser::kFastWindow);
    REQUIRE(analyser.transform());
    CHECK(analyser.band_db_at_note(69) < -100.0f);

    analyser.advance(1.0);
    CHECK(analyser.peak_db_at_note(69) == doctest::Approx(played - 20.0f).epsilon(0.02));

    // And it stops falling at the floor rather than running to negative infinity, because the panel draws
    // the peak as a height and a height below the panel is not a height.
    analyser.advance(60.0);
    CHECK(analyser.peak_db_at_note(69) >= Analyser::kFloorDb);
}

TEST_CASE("a transform needs a window of new sound, and a drainer that is late counts what it missed") {
    Analyser analyser(Analyser::kFastWindow, kRate);
    std::vector<float> block(64 * 2, 0.0f);
    analyser.push(block.data(), 64);
    CHECK_FALSE(analyser.transform()); // less than a window pushed
    analyser.push(block.data(), Analyser::kFastWindow - 64);
    CHECK(analyser.transform());
    CHECK_FALSE(analyser.transform()); // nothing new since the last one
    CHECK(analyser.transform_count() == 1);

    analyser.push(block.data(), Analyser::kFastWindow + 100); // the frame loop was away
    CHECK(analyser.transform());
    CHECK(analyser.dropped_input_frames() == 100);
}

TEST_CASE("the tap hands over what it still holds, oldest first") {
    SampleTap tap;
    std::vector<float> block(512 * 2);
    for (int pass = 0; pass < 3; ++pass) {
        for (int i = 0; i < 512; ++i) {
            const float marker = static_cast<float>(pass * 512 + i);
            block[static_cast<std::size_t>(i * 2)] = marker;
            block[static_cast<std::size_t>(i * 2 + 1)] = -marker;
        }
        tap.write(block.data(), 512);
    }
    CHECK(tap.pending_frames() == 3 * 512);
    CHECK(tap.dropped_frames() == 0);

    std::vector<float> got(1024 * 2);
    const int taken = tap.read(got.data(), 1024);
    REQUIRE(taken == 1024);
    for (int i = 0; i < taken; ++i) {
        const float marker = got[static_cast<std::size_t>(i * 2)];
        CHECK(marker == static_cast<float>(i)); // everything written is still held: nothing was overtaken
        CHECK(got[static_cast<std::size_t>(i * 2 + 1)] == -marker);
    }
    CHECK(tap.read(got.data(), 1024) == 512);
    CHECK(got[0] == 1024.0f); // and the rest follows where the first read stopped
    CHECK(tap.pending_frames() == 0);
}

TEST_CASE("ten thousand blocks with nobody draining neither block nor grow the ring") {
    SampleTap tap;
    std::vector<float> block(512 * 2, 0.25f);
    for (int pass = 0; pass < 10000; ++pass) {
        tap.write(block.data(), 512); // the frame loop is asleep; this must not wait
        REQUIRE(tap.pending_frames() <= SampleTap::kCapacityFrames);
    }
    CHECK(tap.written_frames() == 10000 * 512);
    CHECK(tap.pending_frames() == SampleTap::kCapacityFrames);

    // What is left is the sound that is happening now, which is the only thing a spectrum is for. The
    // frames that were thrown away are counted when the reader comes back, because the reader is the one
    // that can say what it missed.
    std::vector<float> got(static_cast<std::size_t>(SampleTap::kCapacityFrames) * 2);
    const int taken = tap.read(got.data(), SampleTap::kCapacityFrames);
    REQUIRE(taken == SampleTap::kCapacityFrames);
    CHECK(tap.dropped_frames() == 10000 * 512 - SampleTap::kCapacityFrames);
    CHECK(got[static_cast<std::size_t>((SampleTap::kCapacityFrames - 1) * 2)] == 0.25f);
}

TEST_CASE("a hidden panel drains the sound and forgets the picture") {
    // What the app does on every frame the spectrum is not the panel on screen: keep the ring empty, so
    // the audio thread's copy never has to throw anything away, and reset the analyser, so switching the
    // panel back starts from silence instead of from the last frame it happened to draw. A meter frozen
    // at the level of a minute ago is a meter saying "nothing is playing", and that is not what it knows.
    SampleTap tap;
    Analyser analyser(2048, kRate);

    // Fewer frames than the ring holds, so "nothing was dropped" means the panel was away and not slow.
    std::vector<float> block(512 * 2, 0.25f);
    for (int i = 0; i < 12; ++i)
        tap.write(block.data(), 512);
    std::vector<float> drain(static_cast<std::size_t>(SampleTap::kCapacityFrames) * 2);
    REQUIRE(tap.read(drain.data(), tap.pending_frames()) == 12 * 512);
    CHECK(tap.pending_frames() == 0);
    CHECK(tap.dropped_frames() == 0); // drained, not dropped: the panel was away, not slow

    // A picture exists: A4, loud, with a hold line above it.
    push_one_window_of_sine(analyser, note_hz(69), 0.1);
    REQUIRE(analyser.transform());
    analyser.advance(0.0);
    analyser.advance(1.0);
    REQUIRE(analyser.band_db_at_note(69) > -30.0f);
    REQUIRE(analyser.peak_db_at_note(69) > -30.0f);

    analyser.reset();
    for (int band = 0; band < Analyser::band_count(); ++band) {
        CHECK(analyser.band_db(band) == Analyser::kFloorDb);
        CHECK(analyser.peak_db(band) == Analyser::kFloorDb);
    }

    // And it still works afterwards: a reset is not a dead instrument. A2 lights its own band.
    push_one_window_of_sine(analyser, note_hz(45), 0.1);
    REQUIRE(analyser.transform());
    CHECK(std::abs(level_at_note(analyser, 45) - -20.0f) <= 1.5f);
    // The old picture is gone, not merely dimmed: A4 sits far below the A2 that is actually sounding.
    CHECK(analyser.band_db_at_note(69) < level_at_note(analyser, 45) - 40.0f);
}
