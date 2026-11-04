#pragma once

#include <array>
#include <cmath>

namespace boreaz::dsp {

namespace detail {

// sin() at compile time, to about 7e-10 on the folded range: a 13th-order odd Taylor whose next
// term is x^15/15!. It exists so the table below is a constant rather than something a static
// initialiser builds before main.
constexpr double sine_exact(double x) noexcept {
    constexpr double kPi = 3.14159265358979311600;
    constexpr double kHalfPi = 1.57079632679489655800;
    int sign = 1;
    while (x >= kPi) { // at most a couple of folds for the angles the table asks for
        x -= kPi;
        sign = -sign;
    }
    if (x > kHalfPi)
        x = kPi - x; // sin(pi - x) == sin(x)
    const double x2 = x * x;
    double p = 1.6059043836821613e-10;  //  1/13!
    p = p * x2 - 2.5052108385441719e-8; //  1/11!
    p = p * x2 + 2.7557319223985893e-6; //  1/9!
    p = p * x2 - 1.9841269841269840e-4; //  1/7!
    p = p * x2 + 8.3333333333333332e-3; //  1/5!
    p = p * x2 - 1.6666666666666666e-1; //  1/3!
    p = p * x2 + 1.0;
    return static_cast<double>(sign) * x * p;
}

// One cycle of the sine in 4096 points: 16 KB of .rodata, shared by every voice and every thread.
// Same order of size as the pitch LUT the sampler already carries, and read the same way.
inline constexpr int kSineEntries = 4096;
static_assert((kSineEntries & (kSineEntries - 1)) == 0,
              "the wrap in sine_at is a mask, so the table has to be a power of two");
inline constexpr std::array<float, kSineEntries> kSineTable = [] {
    std::array<float, kSineEntries> table{};
    for (int i = 0; i < kSineEntries; ++i)
        table[static_cast<std::size_t>(i)] = static_cast<float>(sine_exact(
            6.28318530717958647692 * static_cast<double>(i) / static_cast<double>(kSineEntries)));
    return table;
}();

// The read itself, out of the switch so a case label stays one line. Interpolating in float keeps
// the multiply-add cheap, and the phase is a float anyway, so the fraction is already quantised to
// about that much. The mask, not a modulo, wraps the last point of the cycle onto the first - which
// is also why a phase that lands on exactly 1.0 reads table[0] instead of running off the end of
// the table.
inline float sine_at(float phase) noexcept {
    const float scaled = phase * static_cast<float>(kSineEntries);
    const int index = static_cast<int>(scaled);
    const float fraction = scaled - static_cast<float>(index);
    const float here = kSineTable[static_cast<std::size_t>(index & (kSineEntries - 1))];
    const float next = kSineTable[static_cast<std::size_t>((index + 1) & (kSineEntries - 1))];
    return here + fraction * (next - here);
}

} // namespace detail

enum class Waveform { Sine, Saw, Square, Triangle };

// Value of `waveform` at `phase`, where phase is in [0, 1). Stateless so the tests and the
// future wavetable/polyBLEP work can share one definition.
//
// The sine is a table read with linear interpolation, not std::sin. That is a CPU measure and not a
// change of waveform: the sine is already exact, and two evaluations of it per frame per voice cost
// 96 ns of the 384 ns the budget is made of (PLAN.md §7, engine cost). Measured against a double
// reference, the largest error over the whole cycle is 4.8e-7, where the float reference itself
// carries 4.0e-7, and a 4096-point table read takes 2.9 ns against 48 ns.
//
// shortcut: saw, square, and triangle are naive and alias above roughly 2 kHz; polyBLEP
// arrives with the voice phase (PLAN.md §4), which replaces those three and nothing else.
inline float waveform_at(Waveform waveform, float phase) noexcept {
    // pi-lens-ignore: non-case-label-in-switch -- a MISRA C query run against a C++ header: the C
    // grammar cannot parse `case Waveform::Sine:` and reads the enumerator as a plain label. clangd
    // reports nothing here and the file compiles as C++20; the same four lines are unchanged since
    // the oscillator landed in phase 3.
    switch (waveform) {
        case Waveform::Sine:
            return detail::sine_at(phase);
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

    // Advance `frames` without producing samples, and return the waveform's value where the block
    // began. A block-rate LFO wants one value per block, not the 64 `step` would evaluate, and the
    // real-time rules already update LFO increments once per block for the same reason. The phase
    // is wrapped with floor rather than the single subtract `step` gets away with, because a block
    // spans many cycles of a fast LFO.
    float advance_block(int frames) noexcept {
        const float current = phase_;
        phase_ = current + increment_ * static_cast<float>(frames);
        phase_ -= std::floor(phase_);
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
