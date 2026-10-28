#pragma once

#include <cmath>

namespace boreaz::dsp {

// Linear gain with a one-block ramp. The ramp is not decoration: a gain that jumps between
// two values once per control-rate update is a click, and every knob in the GUI ends up
// here.
class Gain {
  public:
    void set_linear(float linear) noexcept { target_ = linear; }

    // Called from the GUI and from parameter loading, never from the audio callback: pow()
    // there would be the usual self-inflicted wound.
    void set_gain_db(float db) noexcept {
        target_ = static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
    }

    float target() const noexcept { return target_; }
    float current() const noexcept { return current_; }

    float process(float in) noexcept {
        current_ = target_;
        return in * current_;
    }

    void process_block(float* buffer, int frames) noexcept {
        if (frames <= 0) {
            return;
        }
        const float from = current_;
        if (from == target_) {
            for (int i = 0; i < frames; ++i) {
                buffer[i] *= target_;
            }
            return;
        }
        const float step = (target_ - from) / static_cast<float>(frames);
        float value = from;
        for (int i = 0; i < frames; ++i) {
            value += step;
            buffer[i] *= value;
        }
        current_ = target_;
    }

    void reset() noexcept {
        current_ = 1.0f;
        target_ = 1.0f;
    }

  private:
    float current_ = 1.0f;
    float target_ = 1.0f;
};

// tanh saturator. It keeps the waveform continuous when a voice stack overloads the bus;
// the thing that guarantees nothing passes +/-1.0 is the master limiter, which is a later
// phase (PLAN.md §3) and sits after this.
//
// The coefficient is smoothed with a one-pole so moving the drive knob does not zipper.
class SoftClip {
  public:
    void prepare(double sample_rate_hz) noexcept {
        // ~5 ms time constant, which is slow enough to hear as a sweep and fast enough to keep
        // transients honest.
        smoothing_ = sample_rate_hz > 0.0
                         ? static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sample_rate_hz)))
                         : 1.0f;
    }

    // drive is the threshold divider: the window is [-drive, +drive] and the output stays
    // inside it, so turning drive up is headroom, not distortion. 1.0 is the bare tanh.
    void set_drive(float drive) noexcept { target_ = drive >= 1.0f ? drive : 1.0f; }

    float process(float in) noexcept {
        coefficient_ += smoothing_ * (target_ - coefficient_);
        return saturate(in / coefficient_) * coefficient_;
    }

    void process_block(float* buffer, int frames) noexcept {
        for (int i = 0; i < frames; ++i) {
            buffer[i] = process(buffer[i]);
        }
    }

    // std::tanh, not an approximation of it: this sits on the master bus, once per sample, and
    // a cheap rational approximation of tanh is off by ~2% near x = 1, which is exactly where a
    // limiter is doing its work. Cost is nothing compared to the branch it would save.
    static float saturate(float x) noexcept { return std::tanh(x); }

    void reset() noexcept { coefficient_ = target_; }
    float drive() const noexcept { return target_; }

  private:
    float target_ = 1.0f;
    float coefficient_ = 1.0f;
    float smoothing_ = 1.0f; // 1.0 means "no smoothing", which is also the unprepared case
};

} // namespace boreaz::dsp
