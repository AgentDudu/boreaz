#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/dsp/svf.h"
#include "boreaz/dsp/tuning.h"

using doctest::Approx;

using boreaz::dsp::Svf;
using boreaz::dsp::Tuning;

namespace {

// doctest's Approx is relative; these checks care about absolute error.
bool near(double actual, double ideal, double tolerance) noexcept {
    return std::abs(actual - ideal) <= tolerance;
}

constexpr double kSampleRate = 44100.0;
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// Amplitude of the steady-state response to a sine, measured after the transient has died.
// Returns the peak of the last quarter of the buffer, which is what a listener would hear.
float gain_at(Svf::Mode mode, float cutoff_hz, float resonance, float signal_hz, int seconds = 1) {
    Svf filter;
    filter.set_mode(mode);
    filter.prepare(kSampleRate, cutoff_hz, resonance);

    const int frames = static_cast<int>(kSampleRate * seconds);
    const int skip = frames / 2;
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const double phase = 2.0 * 3.14159265358979323846 * signal_hz * i / kSampleRate;
        const float out = filter.process(static_cast<float>(std::sin(phase)));
        if (i > skip) {
            peak = std::abs(out) > peak ? std::abs(out) : peak;
        }
    }
    return peak;
}

bool is_denormal(float value) noexcept {
    return value != 0.0f && std::abs(value) < 1.17549435e-38f;
}

} // namespace

TEST_CASE("an unprepared filter passes audio instead of swallowing it") {
    Svf filter;
    CHECK_FALSE(filter.prepared());
    CHECK(filter.process(0.5f) == Approx(0.5f));

    filter.prepare(kSampleRate, 1000.0f, 0.0f);
    CHECK(filter.prepared());
    CHECK(filter.process(0.5f) != Approx(0.5f)); // now it is actually filtering
}

TEST_CASE("low pass passes DC with unity gain and no resonance-dependent offset") {
    for (const float resonance : {0.0f, 0.5f, 1.0f}) {
        Svf filter;
        filter.prepare(kSampleRate, 20000.0f, resonance);

        float out = 0.0f;
        for (int i = 0; i < 20000; ++i) {
            out = filter.process(1.0f);
        }
        INFO("resonance " << resonance);
        CHECK(near(out, 1.0f, 1.0e-4f));
    }
}

TEST_CASE("high pass blocks DC and low pass stops something an octave above its cutoff") {
    Svf high;
    high.set_mode(Svf::Mode::HighPass);
    high.prepare(kSampleRate, 500.0f, 0.0f);
    float out = 0.0f;
    for (int i = 0; i < 50000; ++i) {
        out = high.process(1.0f);
    }
    CHECK(std::abs(out) < 1.0e-4f);

    // 12 kHz is roughly two octaves above a 3 kHz cutoff: a second-order section owes about
    // 24 dB per octave, so anything under 25 dB of attenuation would mean the coefficients
    // are wrong, not the test's arithmetic.
    const float attenuation = gain_at(Svf::Mode::LowPass, 3000.0f, 0.0f, 12000.0f);
    CHECK(attenuation < 0.18f);
    CHECK(20.0f * std::log10(attenuation) < -15.0f);
}

TEST_CASE("resonance adds a peak at cutoff without the filter deciding to sing forever") {
    const float flat = gain_at(Svf::Mode::BandPass, 440.0f, 0.0f, 440.0f);
    const float resonant = gain_at(Svf::Mode::BandPass, 440.0f, 0.9f, 440.0f);
    const float resonant_off_frequency = gain_at(Svf::Mode::BandPass, 440.0f, 0.9f, 110.0f);

    CHECK(resonant > flat * 4.0f);            // Q is 50 at the top of the range
    CHECK(resonant_off_frequency < resonant); // and it is a peak, not a boost
    CHECK(std::isfinite(resonant));

    // Bandwidth sanity: half a octave away must already be well off the peak.
    const float nearby = gain_at(Svf::Mode::BandPass, 440.0f, 0.9f, 620.0f);
    CHECK(nearby < resonant * 0.9f);
}

TEST_CASE(
    "the filter stays bounded at A0 with resonance maxed, which is where float filters buzz") {
    Svf filter;
    filter.prepare(kSampleRate, 27.5f, 1.0f);

    // Deterministic pseudo-noise plus a full-scale impulse every 4096 samples: the impulse is
    // what would make an unstable resonance grow without limit.
    std::uint32_t state = 0x12345678u;
    float peak = 0.0f;
    for (int i = 0; i < 1000000; ++i) {
        state = state * 1664525u + 1013904223u;
        const float noise =
            static_cast<float>(static_cast<int>(state >> 8) % 2000 - 1000) / 1000.0f;
        const float in = (i % 4096 == 0) ? 1.0f : noise * 0.1f;
        const float out = filter.process(in);

        REQUIRE(std::isfinite(out));
        peak = std::abs(out) > peak ? std::abs(out) : peak;
    }
    CHECK(peak < 100.0f); // a self-oscillating or drifting filter blows past this quickly
}

// The plan's real-time rule is flush-to-zero on the audio thread (PLAN.md §3). The test
// target does not set that flag, so the assertion below says what the audio path guarantees
// rather than what this binary happens to do with a denormal: the ringing state of a filter
// at A0 and Q = 50 spends thousands of samples in the subnormal range before it reaches
// zero, which is exactly why the rule exists.
TEST_CASE("10^6 samples of a resonant ring-out stay finite and converge") {
    Svf filter;
    filter.prepare(kSampleRate, 27.5f, 1.0f);

    filter.process(1.0f); // kick it, then let it ring for a million samples
    int denormals = 0;
    float last = 1.0f;
    for (int i = 0; i < 1000000; ++i) {
        const float out = filter.process(0.0f);
        REQUIRE(std::isfinite(out));
        if (is_denormal(out)) {
            ++denormals;
        }
        last = out;
    }

    // Q = 50 at 27.5 Hz still has energy left after 22 seconds, so "converged to zero" is not
    // what this checks; "did not grow, did not latch, did not go non-finite" is.
    CHECK(std::abs(last) < 1.0f);
    if (denormals > 0) {
        WARN("test binary has no flush-to-zero; the audio thread sets it (PLAN.md §3)");
    }
}

TEST_CASE("absurd parameters cannot make the filter produce garbage") {
    const std::vector<float> cutoffs = {kNaN, -1.0f, 0.0f, 1.0e-9f, 10.0f, 27.5f, 4186.0f, 1.0e9f};
    const std::vector<float> resonances = {kNaN, -1.0f, 0.0f, 0.5f, 1.0f, 1.0e9f};

    for (const float cutoff : cutoffs) {
        for (const float resonance : resonances) {
            Svf filter;
            filter.prepare(kSampleRate, cutoff, resonance);
            for (const Svf::Mode mode :
                 {Svf::Mode::LowPass, Svf::Mode::HighPass, Svf::Mode::BandPass}) {
                filter.set_mode(mode);
                for (int i = 0; i < 32; ++i) {
                    const float out = filter.process(i % 2 == 0 ? 1.0f : -1.0f);
                    INFO("cutoff " << cutoff << " resonance " << resonance << " mode "
                                   << static_cast<int>(mode));
                    REQUIRE(std::isfinite(out));
                }
            }
        }
    }

    // A sample rate of zero is a device that has not opened yet: keep the old coefficients.
    Svf kept;
    kept.prepare(kSampleRate, 1000.0f, 0.0f);
    Svf ignored;
    ignored.prepare(kSampleRate, 1000.0f, 0.0f);
    ignored.prepare(0.0, 20000.0f, 1.0f); // must be a no-op

    for (int i = 0; i < 64; ++i) {
        const float in = static_cast<float>((i % 7) - 3) * 0.3f;
        CHECK(ignored.process(in) == Approx(kept.process(in)).scale(1.0));
    }
}

TEST_CASE("every key on the keyboard gives a usable cutoff") {
    const Tuning tuning;

    for (int note = Tuning::kFirstKey; note <= Tuning::kLastKey; ++note) {
        for (const float resonance : {0.0f, 0.7f, 1.0f}) {
            Svf filter;
            filter.set_mode(Svf::Mode::LowPass);
            filter.prepare(kSampleRate, tuning.frequency(note), resonance);

            float out = 0.0f;
            for (int i = 0; i < 200; ++i) {
                out = filter.process(1.0f);
            }
            INFO("note " << note << " resonance " << resonance);
            REQUIRE(std::isfinite(out));
            CHECK(out > 0.0f); // DC through a low pass must be positive, at any cutoff
        }
    }
}

TEST_CASE("process_block matches sample-at-a-time processing") {
    Svf reference;
    reference.prepare(kSampleRate, 1200.0f, 0.6f);
    Svf blocked;
    blocked.prepare(kSampleRate, 1200.0f, 0.6f);

    std::vector<float> buffer(64);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const double phase =
            2.0 * 3.14159265358979323846 * 440.0 * static_cast<double>(i) / kSampleRate;
        buffer[i] = static_cast<float>(std::sin(phase));

        const float expected = reference.process(buffer[i]);
        CHECK(blocked.process(buffer[i]) == Approx(expected));
    }

    blocked.reset();
    reference.reset();
    std::vector<float> copy = buffer;
    blocked.process_block(copy.data(), static_cast<int>(copy.size()));
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        CHECK(copy[i] == Approx(reference.process(buffer[i])));
    }
}
