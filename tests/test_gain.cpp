#include <cmath>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/dsp/gain.h"

using doctest::Approx;

using boreaz::dsp::Gain;
using boreaz::dsp::SoftClip;

namespace {

// doctest's Approx is relative; these checks care about absolute error.
bool near(double actual, double ideal, double tolerance) noexcept {
    return std::abs(actual - ideal) <= tolerance;
}

} // namespace

TEST_CASE("gain of one is the signal, and zero is silence") {
    Gain gain;
    CHECK(gain.process(0.25f) == Approx(0.25f));

    gain.set_linear(0.0f);
    CHECK(gain.process(0.25f) == 0.0f);
}

TEST_CASE("decibels land where a mixer says they land") {
    Gain gain;

    gain.set_gain_db(0.0f);
    CHECK(gain.target() == Approx(1.0f));

    gain.set_gain_db(-6.0f);
    CHECK(gain.target() == Approx(0.5011872f).epsilon(1.0e-5f));

    gain.set_gain_db(-24.0f);
    CHECK(gain.target() == Approx(0.0630957f).epsilon(1.0e-5f));

    gain.set_gain_db(6.0f);
    CHECK(gain.target() == Approx(1.9952623f).epsilon(1.0e-5f));

    float out = 0.0f;
    for (int i = 0; i < 64; ++i) {
        out = gain.process(1.0f); // one block, so the ramp has arrived
    }
    CHECK(out == Approx(1.9952623f).epsilon(1.0e-5f));
}

TEST_CASE("a gain change ramps across the block instead of clicking") {
    Gain gain;
    gain.set_linear(1.0f);
    gain.process(1.0f); // settle

    gain.set_linear(0.0f);

    std::vector<float> buffer(64, 1.0f);
    gain.process_block(buffer.data(), static_cast<int>(buffer.size()));

    float previous = 1.0f;
    for (const float value : buffer) {
        REQUIRE(value <= previous + 1.0e-6f); // monotonic down
        REQUIRE(previous - value < 0.05f);    // no step big enough to hear as a click
        previous = value;
    }
    CHECK(near(buffer.back(), 0.0f, 1.0e-6f));
    CHECK(gain.current() == Approx(0.0f));
}

TEST_CASE("an unchanged gain is applied exactly, block after block") {
    Gain gain;
    gain.set_linear(0.5f);

    std::vector<float> warm = {1.0f, -1.0f, 0.25f, -0.75f};
    gain.process_block(warm.data(), 4); // the first block is the ramp; later blocks are exact

    for (int block = 0; block < 4; ++block) {
        std::vector<float> buffer = {1.0f, -1.0f, 0.25f, -0.75f};
        gain.process_block(buffer.data(), static_cast<int>(buffer.size()));
        CHECK(buffer[0] == Approx(0.5f));
        CHECK(buffer[1] == Approx(-0.5f));
        CHECK(buffer[2] == Approx(0.125f));
        CHECK(buffer[3] == Approx(-0.375f));
    }
}

TEST_CASE("soft clip never exceeds its drive and stays odd and monotonic") {
    SoftClip clipper;
    clipper.prepare(44100.0);

    for (const float drive : {1.0f, 2.0f, 6.0f}) {
        clipper.set_drive(drive);
        clipper.reset();
        for (int i = 0; i < 5000; ++i) {
            clipper.process(0.0f); // let the one-pole arrive before judging the curve's shape
        }

        float previous = -1.0e9f; // monotonic for a fixed drive; changing drive moves the curve
        for (float in = -20.0f; in <= 20.0f; in += 0.01f) {
            const float out = clipper.process(in);
            REQUIRE(std::isfinite(out));
            REQUIRE(std::abs(out) <= drive + 1.0e-4f);
            REQUIRE(out >= previous); // monotonic: a saturator that turns back is broken
            previous = out;
        }
    }

    // set_drive before reset: reset parks the smoothed coefficient at the target, so the two
    // halves of an odd-symmetry check have to start from the same coefficient.
    clipper.set_drive(1.0f);
    clipper.reset();
    const float positive = clipper.process(0.3f);
    clipper.reset();
    const float negative = clipper.process(-0.3f);
    CHECK(near(positive, -negative, 1.0e-6f));
}

TEST_CASE("soft clip is a straight line for small signals and tanh for medium ones") {
    SoftClip clipper;
    clipper.prepare(44100.0);
    clipper.set_drive(1.0f);
    clipper.reset();

    // Near zero the curve has to be unity gain or quiet passages get squashed.
    const float small = clipper.process(0.001f);
    CHECK(near(small, 0.001f, 1.0e-6f));

    clipper.reset();
    const float medium = clipper.process(0.4f);
    CHECK(near(medium, std::tanh(0.4f), 1.0e-5f));

    clipper.reset();
    CHECK(near(clipper.process(1.0f), std::tanh(1.0f), 1.0e-5f));
}

TEST_CASE("drive below unity is not a thing, and absurd input is still finite") {
    SoftClip clipper;
    clipper.prepare(44100.0);

    clipper.set_drive(0.1f);
    CHECK(clipper.drive() == 1.0f);

    clipper.set_drive(1.0f);
    clipper.reset();
    CHECK(std::isfinite(clipper.process(1.0e30f)));
    CHECK(std::isfinite(clipper.process(-1.0e30f)));
    CHECK(std::abs(clipper.saturate(1.0e30f)) <= 1.0f);
    CHECK(SoftClip::saturate(0.0f) == 0.0f);
}

TEST_CASE("no sample rate means no smoothing, which is still safe") {
    SoftClip clipper;
    clipper.set_drive(3.0f); // prepare() deliberately skipped
    for (int i = 0; i < 10; ++i) {
        const float out = clipper.process(2.0f);
        REQUIRE(std::isfinite(out));
        CHECK(std::abs(out) <= 3.0f + 1.0e-4f);
    }
}

TEST_CASE("a million samples of noise through gain into soft clip stay finite and bounded") {
    Gain gain;
    gain.set_linear(4.0f);
    SoftClip clipper;
    clipper.prepare(44100.0);
    clipper.set_drive(1.0f);
    clipper.reset();

    std::uint32_t state = 0xdeadbeefu;
    for (int i = 0; i < 1000000; ++i) {
        state = state * 1664525u + 1013904223u;
        const float noise =
            static_cast<float>(static_cast<int>(state >> 8) % 2000 - 1000) / 1000.0f;

        float sample = noise;
        gain.process_block(&sample, 1);
        const float out = clipper.process(sample);

        REQUIRE(std::isfinite(out));
        CHECK(std::abs(out) <= 1.0f + 1.0e-4f);
    }
}
