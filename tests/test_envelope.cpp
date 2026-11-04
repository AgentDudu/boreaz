#include <cmath>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/dsp/envelope.h"

using doctest::Approx;

using boreaz::dsp::Adsr;
using Stage = boreaz::dsp::Adsr::Stage;

namespace {

constexpr float kSampleRate = 1000.0f; // one sample per millisecond makes the counts readable

Adsr make(float attack, float decay, float sustain, float release, float curvature = 0.0f) {
    Adsr envelope;
    envelope.prepare(kSampleRate);
    envelope.set_times(attack, decay, sustain, release);
    envelope.set_curvature(curvature);
    return envelope;
}

} // namespace

TEST_CASE("segment lengths are exact in samples, which is what the plan promises") {
    auto envelope = make(0.01f, 0.02f, 0.5f, 0.04f); // 10 / 20 / 40 samples

    envelope.gate_on();

    // Attack: reaches 1.0 on the 10th sample, not the 9th and not the 11th.
    float value = 0.0f;
    int attack_samples = 0;
    for (int i = 1; i <= 100; ++i) {
        value = envelope.process();
        if (value >= 1.0f) {
            attack_samples = i;
            break;
        }
    }
    CHECK(attack_samples == 10);

    // Decay: 20 more samples down to the sustain level.
    int decay_samples = 0;
    for (int i = 1; i <= 100; ++i) {
        value = envelope.process();
        if (value <= 0.5f) {
            decay_samples = i;
            break;
        }
    }
    CHECK(decay_samples == 20);
    CHECK(envelope.stage() == Stage::Sustain);

    // Release: 40 samples to silence, then Idle.
    envelope.gate_off();
    int release_samples = 0;
    for (int i = 1; i <= 100; ++i) {
        value = envelope.process();
        if (value <= 0.0f) {
            release_samples = i;
            break;
        }
    }
    CHECK(release_samples == 40);
    CHECK(envelope.stage() == Stage::Idle);
    CHECK(envelope.value() == 0.0f);
    CHECK_FALSE(envelope.active());
}

TEST_CASE("curvature changes the shape and never changes the duration") {
    for (const float curvature : {0.0f, 0.5f, 4.0f, 12.0f}) {
        auto envelope = make(0.01f, 0.02f, 0.5f, 0.04f, curvature);
        INFO("curvature " << curvature);
        REQUIRE(envelope.segment_samples(Stage::Attack) == 10);

        envelope.gate_on();
        float value = 0.0f;
        int attack_samples = 0;
        for (int i = 1; i <= 100; ++i) {
            value = envelope.process();
            if (value >= 1.0f) {
                attack_samples = i;
                break;
            }
        }
        CHECK(attack_samples == 10);

        // Curved segments are curved: the first sample has covered more than an eighth of the
        // distance, which a straight line cannot do in one sample of ten.
        if (curvature > 1.0f) {
            auto shaped = make(0.01f, 0.02f, 0.5f, 0.04f, curvature);
            auto linear = make(0.01f, 0.02f, 0.5f, 0.04f, 0.0f);
            shaped.gate_on();
            linear.gate_on();
            CHECK(shaped.process() > linear.process() * 2.0f);
        }
    }
}

TEST_CASE("sustain holds exactly the level, forever, without drift") {
    auto envelope = make(0.001f, 0.001f, 0.73f, 0.01f);
    envelope.gate_on();

    for (int i = 0; i < 1000; ++i) {
        envelope.process();
    }
    REQUIRE(envelope.stage() == Stage::Sustain);

    for (int i = 0; i < 10000; ++i) {
        REQUIRE(envelope.process() == Approx(0.73f));
    }
}

TEST_CASE("retriggering starts from where the envelope is, never from zero") {
    auto envelope = make(0.01f, 0.1f, 0.5f, 0.1f);
    envelope.gate_on();

    float before = 0.0f;
    for (int i = 0; i < 30; ++i) { // 10 up, 20 into the decay
        before = envelope.process();
    }
    REQUIRE(before > 0.5f);
    REQUIRE(before < 1.0f);

    envelope.gate_on();
    const float after = envelope.process();

    CHECK(after > before);                          // it went back up
    CHECK(after - before < (1.0f - before) * 0.5f); // and did not jump there
    CHECK(envelope.stage() == Stage::Attack);
}

TEST_CASE("a note released during the attack still gets a full release") {
    auto envelope = make(0.02f, 0.1f, 0.5f, 0.05f);
    envelope.gate_on();
    float value = 0.0f;
    for (int i = 0; i < 5; ++i) {
        value = envelope.process();
    }
    REQUIRE(envelope.stage() == Stage::Attack);
    REQUIRE(value > 0.0f);
    REQUIRE(value < 1.0f);

    envelope.gate_off();
    CHECK(envelope.stage() == Stage::Release);

    const float first = envelope.process();
    CHECK(first < value); // falls from the value it was released at

    float last = 1.0f;
    int samples = 0;
    for (int i = 1; i <= 200; ++i) {
        last = envelope.process();
        if (last <= 0.0f) {
            samples = i;
            break;
        }
    }
    // 50-sample release, minus the one sample this test already consumed above.
    CHECK(samples == 49);
    CHECK(last == 0.0f);
}

TEST_CASE("gates in the wrong order do nothing") {
    auto envelope = make(0.01f, 0.01f, 0.5f, 0.01f);

    envelope.gate_off(); // never gate on
    CHECK(envelope.stage() == Stage::Idle);
    CHECK(envelope.process() == 0.0f);
    envelope.gate_off();
    CHECK(envelope.stage() == Stage::Idle);
    CHECK(envelope.value() == 0.0f);
}

TEST_CASE("zero-length times mean jump, not a divide by zero and not a stuck voice") {
    auto envelope = make(0.0f, 0.0f, 1.0f, 0.0f);
    envelope.gate_on();

    CHECK(envelope.process() == Approx(1.0f)); // attack and decay collapse; sustain is 1.0
    CHECK(envelope.stage() == Stage::Sustain);

    envelope.gate_off();
    CHECK(envelope.process() == 0.0f);
    CHECK(envelope.stage() == Stage::Idle);
}

TEST_CASE("durations round to whole samples") {
    auto envelope = make(0.0005f, 0.0015f, 0.25f, 0.0005f); // 0.5, 1.5, 0.5 samples

    CHECK(envelope.segment_samples(Stage::Attack) == 1);
    CHECK(envelope.segment_samples(Stage::Decay) == 2);
    CHECK(envelope.segment_samples(Stage::Release) == 1);
    CHECK(envelope.segment_samples(Stage::Sustain) == -1); // not timed
}

TEST_CASE("an envelope prepared with no sample rate lands on its stages instantly") {
    Adsr envelope;
    envelope.set_times(0.01f, 0.01f, 0.5f, 0.01f);
    envelope.gate_on();

    // No sample rate means every segment is instantaneous, so the envelope is already holding
    // sustain rather than pretending to be in attack.
    CHECK(envelope.process() == Approx(0.5f));
    CHECK(envelope.stage() == Stage::Sustain);

    envelope.gate_off();
    CHECK(envelope.process() == 0.0f);
    CHECK(envelope.stage() == Stage::Idle);
}

TEST_CASE("a million mixed gate events stay inside [0, 1] and stay finite") {
    auto envelope = make(0.001f, 0.05f, 0.6f, 0.02f, 6.0f);

    std::uint32_t state = 0x2b1b30u;
    for (int i = 0; i < 1000000; ++i) {
        state = state * 1664525u + 1013904223u;
        if ((state & 0x3ffu) == 0) {
            (state & 0x1000u) ? envelope.gate_on() : envelope.gate_off();
        }
        const float value = envelope.process();
        REQUIRE(std::isfinite(value));
        REQUIRE(value >= 0.0f);
        REQUIRE(value <= 1.0f);
    }
}

TEST_CASE("the block overload is the same envelope as the sample overload") {
    auto sample_wise = make(0.003f, 0.007f, 0.4f, 0.005f, 5.0f);
    auto blocked = sample_wise;

    sample_wise.gate_on();
    blocked.gate_on();

    std::vector<float> out(64);
    for (int block = 0; block < 8; ++block) {
        blocked.process(out.data(), static_cast<int>(out.size()));
        for (const float value : out) {
            CHECK(sample_wise.process() == Approx(value));
        }
    }
}
