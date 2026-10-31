#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace boreaz::dsp {

// A stereo delay line: two taps that remember, no modulation, no filtering, no crossfeed.
//
// That is the whole effect, and it is on purpose. A delay is the one FX whose every number can be
// asserted — an impulse comes out `n` samples later, scaled by exactly the send — so it is the
// effect that proves the bus around it is wired right before a reverb arrives to make fuzzy claims
// about tails.
//
// The tap is a whole number of samples. A fractional tap needs interpolation, and interpolation in
// a delay is what turns a knob move into a pitch bend; the time only moves when a knob moves, so
// the honest cost of that would be paid by whoever turns it.
//
// `shortcut`: integer taps, no input filtering and no feedback damping; upgrade when someone wants
// a modulated or a tape-style delay, which needs interpolation and a one-pole in the loop.
class Delay {
  public:
    // Sized at prepare time so nothing in the audio path allocates, which is the same rule the
    // voice pool and the engine's scratch buffers follow. Half a second is a long slap; a room is
    // what the reverb is for.
    static constexpr double kMaxDelaySeconds = 0.5;
    // Below this a tail is -600 dBFS and the only thing left for it to do is become a denormal and
    // burn CPU feeding itself. Flushing there is inaudible by about ten orders of magnitude.
    static constexpr float kSilenceFloor = 1e-30f;

    // Setup path: device open, or an engine being built. `frames` in `process` must never exceed
    // what was prepared for, and preparing a second time at a new rate keeps nothing — a delay line
    // measured in samples means nothing at a different rate.
    void prepare(double sample_rate) {
        const double rate = sample_rate > 0.0 ? sample_rate : 44100.0;
        rate_ = rate;
        const std::size_t frames = static_cast<std::size_t>(kMaxDelaySeconds * rate) + 1u;
        line_l_.assign(frames, 0.0f);
        line_r_.assign(frames, 0.0f);
        head_ = 0;
        delay_frames_ = frames_for(delay_seconds_);
    }

    bool ready() const { return !line_l_.empty(); }
    std::size_t size_frames() const { return line_l_.size(); }

    void set_delay_seconds(double seconds) {
        delay_seconds_ = seconds < 0.0 ? 0.0 : (seconds > kMaxDelaySeconds ? kMaxDelaySeconds : seconds);
        delay_frames_ = frames_for(delay_seconds_);
    }
    double delay_seconds() const { return delay_seconds_; }
    std::size_t delay_frames() const { return delay_frames_; }

    // 0.95 is the ceiling, and it is a ceiling on purpose: at 1.0 the line is an oscillator with a
    // knob on it, and no parameter file in this project is allowed to build one.
    void set_feedback(float feedback) {
        feedback_ = feedback < 0.0f ? 0.0f : (feedback > 0.95f ? 0.95f : feedback);
    }
    float feedback() const { return feedback_; }

    // Writes the wet signal only. The dry sum and the send level belong to whoever asked for the
    // effect, so a bypass can be "do not add this" rather than "multiply this by zero" — the
    // difference is that the first one is bit-identical to the effect never existing.
    void process(const float* in_l, const float* in_r, float* wet_l, float* wet_r, int frames) {
        if (frames <= 0)
            return;
        if (!ready()) {
            // Silence, not whatever was in the caller's buffer: an unprepared delay is bypassed, and
            // a bypass that leaks the previous block's wet signal is a stuck echo.
            std::fill_n(wet_l, frames, 0.0f);
            std::fill_n(wet_r, frames, 0.0f);
            return;
        }
        const std::size_t size = line_l_.size();
        const std::size_t d = delay_frames_;
        if (d == 0u) {
            // A ring cannot be tapped zero samples behind itself, so 0 ms means what it sounds like:
            // the wet is the input, and with no tap into the line there are no repeats either.
            for (int i = 0; i < frames; ++i) {
                line_l_[head_] = flush(in_l[i]);
                line_r_[head_] = flush(in_r[i]);
                wet_l[i] = in_l[i];
                wet_r[i] = in_r[i];
                head_ = advance(head_, size);
            }
            return;
        }
        // The tap is derived from the write head rather than stored beside it, so a time change
        // cannot leave the two pointing at each other: one lap of this line is half a second, and a
        // tap that had drifted a lap behind the head is a slap nobody set.
        for (int i = 0; i < frames; ++i) {
            const std::size_t tap = head_ >= d ? head_ - d : head_ + size - d;
            const float out_l = flush(line_l_[tap]);
            const float out_r = flush(line_r_[tap]);
            line_l_[head_] = flush(in_l[i] + out_l * feedback_);
            line_r_[head_] = flush(in_r[i] + out_r * feedback_);
            wet_l[i] = out_l;
            wet_r[i] = out_r;
            head_ = advance(head_, size);
        }
    }

    // True when the line holds nothing, which is how a test asserts that a tail dies instead of
    // merely becoming small.
    bool silent() const {
        for (const float v : line_l_)
            if (v != 0.0f)
                return false;
        for (const float v : line_r_)
            if (v != 0.0f)
                return false;
        return true;
    }

    void reset() {
        std::fill(line_l_.begin(), line_l_.end(), 0.0f);
        std::fill(line_r_.begin(), line_r_.end(), 0.0f);
        head_ = 0;
        delay_frames_ = frames_for(delay_seconds_);
    }

  private:
    static float flush(float v) {
        const float magnitude = v >= 0.0f ? v : -v;
        return magnitude < kSilenceFloor ? 0.0f : v;
    }
    static std::size_t advance(std::size_t index, std::size_t size) {
        return index + 1u < size ? index + 1u : 0u;
    }
    std::size_t frames_for(double seconds) const {
        if (!ready())
            return 0;
        // Nearest sample: a delay line has no fraction of one, and truncating 47.99999 to 47 is a
        // time error nobody asked for.
        std::size_t frames = static_cast<std::size_t>(seconds * rate_ + 0.5);
        if (frames >= line_l_.size())
            frames = line_l_.size() - 1u;
        return frames;
    }

    std::vector<float> line_l_;
    std::vector<float> line_r_;
    std::size_t head_ = 0;
    std::size_t delay_frames_ = 0;
    double rate_ = 44100.0;
    double delay_seconds_ = 0.0;
    float feedback_ = 0.35f;
};

} // namespace boreaz::dsp
