#pragma once

#include <cmath>

namespace boreaz::dsp {

enum class Waveform { Sine, Saw, Square, Triangle };

// Value of `waveform` at `phase`, where phase is in [0, 1). Stateless so the tests and the
// future wavetable/polyBLEP work can share one definition.
//
// shortcut: saw, square, and triangle are naive and alias above roughly 2 kHz; polyBLEP
// arrives with the voice phase (PLAN.md §4), which replaces this and nothing else.
inline float waveform_at(Waveform waveform, float phase) noexcept {
    switch (waveform) {
        case Waveform::Sine:
            return std::sin(6.28318530717958647693f * phase);
        case Waveform::Saw:
            return 2.0f * phase - 1.0f;
        case Waveform::Square:
            return phase < 0.5f ? 1.0f : -1.0f;
        case Waveform::Triangle:
            return phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
    }
    return 0.0f; // unreachable unless a waveform is added without a case above
}

// A phase accumulator: 32-bit phase, one add per sample, no allocation, no branches the
// compiler cannot sink.
class Oscillator {
  public:
    Oscillator() = default;
    explicit Oscillator(Waveform waveform) noexcept : waveform_(waveform) {}

    // Frequencies at or above Nyquist are clamped to just under it. That keeps the increment
    // below 1.0, which is what lets step() get away with a single conditional subtract instead
    // of a fmod.
    void set_frequency(float hz, float sample_rate) noexcept {
        if (!(hz > 0.0f) || !(sample_rate > 0.0f)) {
            frequency_ = 0.0f;
            increment_ = 0.0f;
            return;
        }
        const float limit = 0.4999f * sample_rate;
        frequency_ = hz < limit ? hz : limit;
        increment_ = frequency_ / sample_rate;
    }

    float frequency() const noexcept { return frequency_; }
    float increment() const noexcept { return increment_; }

    void set_waveform(Waveform waveform) noexcept { waveform_ = waveform; }
    Waveform waveform() const noexcept { return waveform_; }

    float phase() const noexcept { return phase_; }
    void set_phase(float phase) noexcept { phase_ = phase - std::floor(phase); }
    void reset() noexcept { phase_ = 0.0f; }

    // Sample at the current phase, then advance.
    float step() noexcept {
        const float current = phase_;
        phase_ = current + increment_;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
        }
        return waveform_at(waveform_, current);
    }

    void render(float* out, int frames) noexcept {
        for (int i = 0; i < frames; ++i) {
            out[i] = step();
        }
    }

  private:
    float phase_ = 0.0f;
    float increment_ = 0.0f;
    float frequency_ = 0.0f;
    Waveform waveform_ = Waveform::Saw;
};

} // namespace boreaz::dsp
