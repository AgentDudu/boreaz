#pragma once

#include <cmath>

namespace boreaz::dsp {

// Chamberlin state-variable filter with trapezoidal integrators: three outputs from one
// pair of states, so a voice that later wants key-tracking plus a morph does not need three
// filters.
//
// Coefficients are computed in double and stored as float (PLAN.md §3): at A0 with high
// resonance a float-only coefficient set rounds its own poles. They are refreshed once per
// block by prepare(), never per sample.
//
// resonance runs 0..1 and maps to the damping coefficient k from 2.0 (Q = 0.5, no peak) down
// to 0.02 (Q = 50, close to self-oscillation). k never reaches zero, so the feedback loop
// cannot latch into a sustained oscillation, which is the one way this topology misbehaves.
class Svf {
  public:
    enum class Mode { LowPass, HighPass, BandPass };

    // A filter that has never been prepared passes audio through. Forgetting prepare() then
    // sounds like no filter, which someone can hear and debug, instead of silence.
    bool prepared() const noexcept { return prepared_; }

    void prepare(double sample_rate_hz, double cutoff_hz, double resonance) noexcept;

    float process(float in) noexcept {
        if (!prepared_) {
            return in;
        }
        const double signal = static_cast<double>(in);

        const double v3 = signal - ic2_;
        const double v1 = a1_ * ic1_ + a2_ * v3;
        const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;

        ic1_ = static_cast<float>(2.0 * v1 - ic1_);
        ic2_ = static_cast<float>(2.0 * v2 - ic2_);

        switch (mode_) {
            case Mode::HighPass:
                return static_cast<float>(signal - k_ * v1 - v2);
            case Mode::BandPass:
                return static_cast<float>(v1);
            case Mode::LowPass:
                break;
        }
        return static_cast<float>(v2);
    }

    void process_block(float* buffer, int frames) noexcept {
        for (int i = 0; i < frames; ++i) {
            buffer[i] = process(buffer[i]);
        }
    }

    void reset() noexcept {
        ic1_ = 0.0f;
        ic2_ = 0.0f;
    }

    void set_mode(Mode mode) noexcept { mode_ = mode; }
    Mode mode() const noexcept { return mode_; }

  private:
    float a1_ = 0.0f;
    float a2_ = 0.0f;
    float a3_ = 0.0f;
    float k_ = 2.0f;

    float ic1_ = 0.0f;
    float ic2_ = 0.0f;

    bool prepared_ = false;

    Mode mode_ = Mode::LowPass;
};

inline void Svf::prepare(double sample_rate_hz, double cutoff_hz, double resonance) noexcept {
    if (!(sample_rate_hz > 0.0)) {
        return; // nothing honest to compute; keep the coefficients we have
    }

    double cutoff = cutoff_hz;
    if (!(cutoff > 1.0e-3)) {
        cutoff = 1.0e-3; // also catches NaN and a negative sweep target
    }
    const double nyquist_guard = 0.49 * sample_rate_hz;
    if (cutoff > nyquist_guard) {
        cutoff = nyquist_guard;
    }

    double r = resonance;
    if (!(r > 0.0)) {
        r = 0.0;
    } else if (r > 1.0) {
        r = 1.0;
    }

    const double g = std::tan(3.14159265358979323846 * cutoff / sample_rate_hz);
    const double k = 2.0 - 1.98 * r;
    const double a1 = 1.0 / (1.0 + g * (g + k));
    const double a2 = g * a1;

    a1_ = static_cast<float>(a1);
    a2_ = static_cast<float>(a2);
    a3_ = static_cast<float>(g * a2);
    k_ = static_cast<float>(k);
    prepared_ = true;
}

} // namespace boreaz::dsp
