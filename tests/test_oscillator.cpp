#include <array>
#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/dsp/oscillator.h"
#include "boreaz/dsp/tuning.h"

using doctest::Approx;

using boreaz::dsp::Oscillator;
using boreaz::dsp::Tuning;
using boreaz::dsp::Waveform;
using boreaz::dsp::waveform_at;

namespace {

// doctest's Approx is relative; these checks care about absolute error.
bool near(double actual, double ideal, double tolerance) noexcept {
    return std::abs(actual - ideal) <= tolerance;
}

constexpr float kSampleRate = 44100.0f;

std::vector<float> render(Waveform waveform, float hz, int frames) {
    Oscillator oscillator(waveform);
    oscillator.set_frequency(hz, kSampleRate);
    std::vector<float> out(static_cast<std::size_t>(frames));
    oscillator.render(out.data(), frames);
    return out;
}

// Whole cycles completed in `frames`, counted from phase wraps.
int cycles(Waveform waveform, float hz, int frames) {
    Oscillator oscillator(waveform);
    oscillator.set_frequency(hz, kSampleRate);
    int wraps = 0;
    for (int i = 0; i < frames; ++i) {
        const float before = oscillator.phase();
        oscillator.step();
        if (oscillator.phase() < before) {
            ++wraps;
        }
    }
    return wraps;
}

} // namespace

TEST_CASE("the oscillator accumulates phase and never leaves [0, 1)") {
    Oscillator oscillator(Waveform::Saw);
    oscillator.set_frequency(1000.0f, kSampleRate);

    for (int i = 0; i < 100000; ++i) {
        oscillator.step();
        REQUIRE(oscillator.phase() >= 0.0f);
        REQUIRE(oscillator.phase() < 1.0f);
    }
}

TEST_CASE("every pitch from A0 to C8 runs at the frequency the tuning table asked for") {
    const Tuning tuning;

    // Two seconds is long enough that a one-cycle counting error is 0.5 Hz: at A0 that is
    // 0.3 cent, well inside the half-cent tolerance, and any real drift shows up immediately.
    constexpr int kFrames = 88200;
    for (const int note : {Tuning::kFirstKey, 40, 60, 69, 90, Tuning::kLastKey}) {
        const float hz = tuning.frequency(note);
        const int completed = cycles(Waveform::Saw, hz, kFrames);

        INFO("note " << note << " at " << hz << " Hz");
        CHECK(std::abs(completed - static_cast<int>(std::lround(static_cast<double>(hz) * 2.0))) <=
              1);
    }
}

TEST_CASE("sine matches std::sin sample for sample") {
    const auto out = render(Waveform::Sine, 440.0f, 256);

    for (std::size_t i = 0; i < out.size(); ++i) {
        const float phase = static_cast<float>(i) * (440.0f / kSampleRate);
        // The oscillator reaches this phase by adding a float increment, so it differs from the
        // exact phase by accumulated rounding; 2e-5 is that drift and nothing more.
        CHECK(near(out[i], std::sin(6.28318530717958647693f * phase), 2.0e-5f));
    }
}

TEST_CASE("saw ramps once per cycle and drops at the wrap") {
    const auto out = render(Waveform::Saw, 1000.0f, 4096);

    float previous = out[0];
    int discontinuities = 0;
    for (const float sample : out) {
        if (sample < previous - 1.5f) {
            ++discontinuities; // the one intentional jump per cycle
        } else {
            CHECK(sample >= previous - 1.0e-6f); // monotonic everywhere else
        }
        previous = sample;
    }
    CHECK(discontinuities > 0);
    CHECK(near(out[0], -1.0f, 1.0e-6f));
}

TEST_CASE("square is 50 percent and triangle stays inside its peaks") {
    const auto square = render(Waveform::Square, 1000.0f, 4410); // ~10 cycles
    int high = 0;
    for (const float sample : square) {
        REQUIRE(std::abs(sample) == Approx(1.0f));
        if (sample > 0.0f) {
            ++high;
        }
    }
    CHECK(high == Approx(static_cast<double>(square.size()) / 2.0).epsilon(0.01));

    const auto triangle = render(Waveform::Triangle, 1000.0f, 4410);
    float minimum = 1.0f;
    float maximum = -1.0f;
    for (const float sample : triangle) {
        minimum = sample < minimum ? sample : minimum;
        maximum = sample > maximum ? sample : maximum;
    }
    CHECK(near(minimum, -1.0f, 0.01f));
    CHECK(near(maximum, 1.0f, 0.01f));
}

TEST_CASE("all four waveforms are finite and bounded across the whole compass") {
    const Tuning tuning;
    const std::array<Waveform, 4> waveforms{Waveform::Sine, Waveform::Saw, Waveform::Square,
                                            Waveform::Triangle};

    for (const Waveform waveform : waveforms) {
        for (int note = 0; note < Tuning::kNoteCount; ++note) {
            const auto out = render(waveform, tuning.frequency(note), 64);
            for (const float sample : out) {
                INFO("waveform " << static_cast<int>(waveform) << " note " << note);
                REQUIRE(sample >= -1.0f);
                REQUIRE(sample <= 1.0f);
                REQUIRE(std::isfinite(sample));
            }
        }
    }
}

TEST_CASE("a frequency above Nyquist is clamped instead of wrapping into garbage") {
    Oscillator oscillator(Waveform::Saw);
    oscillator.set_frequency(100000.0f, kSampleRate);

    CHECK(oscillator.frequency() < 0.5f * kSampleRate);
    CHECK(oscillator.increment() < 1.0f);

    // A single conditional subtract per sample is only correct while the increment is below 1,
    // which is what the clamp above buys us.
    for (int i = 0; i < 1000; ++i) {
        oscillator.step();
        REQUIRE(oscillator.phase() >= 0.0f);
        REQUIRE(oscillator.phase() < 1.0f);
    }
}

TEST_CASE("silent or impossible settings stop the oscillator rather than making noise") {
    Oscillator oscillator(Waveform::Sine);
    oscillator.set_frequency(440.0f, kSampleRate);
    oscillator.step(); // the first sample of a sine starting at phase 0 is exactly zero
    REQUIRE(std::abs(oscillator.step()) > 0.0f);

    oscillator.set_frequency(0.0f, kSampleRate);
    const float first = oscillator.step();
    for (int i = 0; i < 100; ++i) {
        CHECK(oscillator.step() == first);
    }

    oscillator.set_frequency(-5.0f, kSampleRate);
    CHECK(oscillator.frequency() == 0.0f);
    oscillator.set_frequency(440.0f, 0.0f); // a sample rate of zero is not a sample rate
    CHECK(oscillator.increment() == 0.0f);
}

TEST_CASE("phase can be reset and re-seeded, and wraps rather than running away") {
    Oscillator oscillator(Waveform::Saw);
    oscillator.set_frequency(440.0f, kSampleRate);

    oscillator.step();
    oscillator.reset();
    CHECK(oscillator.phase() == 0.0f);
    CHECK(near(oscillator.step(), -1.0f, 1.0e-6f));

    oscillator.set_phase(1.75f);
    CHECK(oscillator.phase() == Approx(0.75f));
    oscillator.set_phase(-0.25f);
    CHECK(oscillator.phase() == Approx(0.75f));
}

TEST_CASE("waveform_at is the single definition used by both the oscillator and the tests") {
    CHECK(waveform_at(Waveform::Square, 0.25f) == 1.0f);
    CHECK(waveform_at(Waveform::Square, 0.75f) == -1.0f);
    CHECK(waveform_at(Waveform::Saw, 0.0f) == -1.0f);
    CHECK(waveform_at(Waveform::Saw, 0.5f) == Approx(0.0f));
    CHECK(waveform_at(Waveform::Triangle, 0.0f) == Approx(-1.0f));
    CHECK(waveform_at(Waveform::Triangle, 0.25f) == Approx(0.0f));
    CHECK(waveform_at(Waveform::Triangle, 0.5f) == Approx(1.0f));
    CHECK(waveform_at(Waveform::Triangle, 0.75f) == Approx(0.0f));
    CHECK(waveform_at(Waveform::Sine, 0.25f) == Approx(1.0f).epsilon(1.0e-6f));
    CHECK(waveform_at(Waveform::Sine, 0.75f) == Approx(-1.0f).epsilon(1.0e-6f));
}
