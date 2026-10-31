// The FX bus, phase 8a: the delay and the bus that holds it.
//
// The delay is the first effect because it is the one whose every number can be asserted: an
// impulse comes out `n` samples later, scaled by exactly the send, and the tail reaches exact zero
// instead of trailing into denormals. The claims here are arithmetic rather than impressions,
// because this bus is where the reverb and the distortion land next.
//
// Deliberately missing, because they belong to an effect that is not built yet: a wet signal with
// any stereo width to it, and any reverb tail at all. The delay is two independent lines with the
// same time and no crossfeed, so a note in one ear never appears in the other, and it is tested as
// such rather than passing for stereo.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "boreaz/dsp/delay.h"
#include "boreaz/engine/engine.h"
#include "boreaz/engine/event.h"
#include "boreaz/engine/fx_bus.h"

namespace {

using boreaz::engine::Engine;
using boreaz::engine::Event;
using boreaz::engine::FxBus;
using boreaz::engine::Params;

constexpr double kRate = 48000.0;

// The engine is exact to the sample and a note's sample is set by which block it lands in, so an
// engine-level lag is allowed three sample-frames: block rounding, plus the tap rounding, plus the
// ramp the master gain does on the first block after a parameter move.
constexpr int kLatencyFrames = 3;

// set_params does not jump. engine.cpp one-poles every parameter toward its target with a 10 ms
// time constant (kParamSlewSeconds), which is what lets a knob move without a click - and what
// makes a test that asks for 120 ms and measures whatever the glide has reached so far fail for a
// reason that has nothing to do with the delay. So every engine-level render below burns this much
// silence first, 25 time constants, and counts from the end of it.
constexpr std::size_t kSettleFrames = 12000; // 250 ms

struct FxBuffer {
    std::vector<float> left;
    std::vector<float> right;
};

// The settle is part of the buffer, so index kSettleFrames is where the note starts.
FxBuffer render(Engine& engine, int seconds) {
    FxBuffer out;
    out.left.resize(kSettleFrames +
                    static_cast<std::size_t>(kRate) * static_cast<std::size_t>(seconds));
    out.right.resize(out.left.size());
    std::vector<float> block(2 * 96);
    for (std::size_t done = 0; done < out.left.size(); done += 96) {
        std::fill(block.begin(), block.end(), 0.0f);
        engine.process(block.data(), 96);
        for (int i = 0; i < 96; ++i) {
            out.left[done + static_cast<std::size_t>(i)] = block[static_cast<std::size_t>(2 * i)];
            out.right[done + static_cast<std::size_t>(i)] =
                block[static_cast<std::size_t>(2 * i + 1)];
        }
    }
    return out;
}

float max_abs(const std::vector<float>& samples) {
    float peak = 0.0f;
    for (const float v : samples)
        peak = std::max(peak, std::fabs(v));
    return peak;
}

// Loudest sample in [from, to), which is how an engine-level test talks about "the echo" without
// claiming a position the block rounding is allowed to move by a sample.
float window_peak(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    float peak = 0.0f;
    for (std::size_t i = from; i < std::min(to, samples.size()); ++i)
        peak = std::max(peak, std::fabs(samples[i]));
    return peak;
}

// A note that is genuinely over, and an echo that is not: with a 5 ms amp release the dry signal is
// gone 50 ms after the note-off, and a 220 ms delay has barely started. Every engine-level claim
// below is then "there is sound where no note is playing", which is the claim, rather than "this
// buffer is different from that buffer", which is not.
Params fx_params() {
    Params p;
    p.amp_release_s = 0.005f;
    p.fx_delay_time_ms = 220.0f;
    p.fx_delay_feedback = 0.0f; // one repeat, so the echo is one event and not a wash
    return p;
}

// One A4, 50 ms long. Ticks are absolute, so the note starts after the settling silence.
void arm(Engine& engine) {
    REQUIRE(engine.push_event(
        Event::note_on(static_cast<std::uint32_t>(kSettleFrames), 0, std::uint8_t(69),
                       std::uint8_t(110))));
    REQUIRE(engine.push_event(Event::note_off(
        static_cast<std::uint32_t>(kSettleFrames + 2400), 0, std::uint8_t(69))));
}

} // namespace

using boreaz::dsp::Delay;

TEST_CASE("delay: the echo is exactly n samples later and exactly as loud as it was told to be") {
    Delay delay;
    delay.prepare(kRate);
    delay.set_delay_seconds(0.01);
    delay.set_feedback(0.0f);
    const std::size_t tap = delay.delay_frames();
    CHECK(tap == 480u); // 10 ms at 48 kHz, and the exactness below depends on that being exact

    std::vector<float> in(2000, 0.0f);
    std::vector<float> silent(2000, 0.0f);
    std::vector<float> wet_l(in.size(), -1.0f);
    std::vector<float> wet_r(in.size(), -1.0f);
    in[100] = 1.0f;
    // The right input is a buffer of its own, and that is the test: handing the same pointer to
    // both inputs would make "the other ear stayed empty" true of a mono delay too.
    delay.process(in.data(), silent.data(), wet_l.data(), wet_r.data(), static_cast<int>(in.size()));

    // Nothing before the tap, and nothing after it either with no feedback: a delay that leaks is
    // a reverb nobody designed.
    CHECK(*std::max_element(wet_l.begin(), wet_l.begin() + 580) == 0.0f);
    CHECK(wet_l[static_cast<std::size_t>(100 + tap)] == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(wet_l[static_cast<std::size_t>(100 + tap + 1)] == 0.0f);
    CHECK(wet_l[static_cast<std::size_t>(100 + 2 * tap)] == 0.0f);

    // Two independent lines with one time: a note in one ear never appears in the other.
    CHECK(*std::max_element(wet_r.begin(), wet_r.end()) == 0.0f);
}

TEST_CASE("delay: feedback is the ratio it is named for, and it stops at exact zero") {
    Delay delay;
    delay.prepare(kRate);
    delay.set_delay_seconds(0.001); // 48 samples, so a dozen repeats fit in the buffer below
    delay.set_feedback(0.5f);
    const std::size_t tap = delay.delay_frames();
    CHECK(tap == 48u);

    std::vector<float> in(1200, 0.0f);
    // Sized for the longest run below, and that is not a detail: passing more frames than the wet
    // buffers hold is a heap overflow, which is how this test first presented itself - as a crash
    // in a later case, with the corrupting write already forgotten.
    // 30000 frames because that is how long dying takes: the decay itself is about 100 repeats of
    // 48 samples, and a cell in the line only gets overwritten - and so flushed - once per lap.
    std::vector<float> wet_l(30000, 0.0f);
    std::vector<float> wet_r(30000, 0.0f);
    in[0] = 1.0f;
    delay.process(in.data(), in.data(), wet_l.data(), wet_r.data(), static_cast<int>(in.size()));

    // The first repeat lands one tap after the impulse, not at it - nothing is heard before the
    // line has been walked - and every round after that is exactly `feedback` of the one before.
    for (int pass = 0; pass < 8; ++pass) {
        const std::size_t at = static_cast<std::size_t>(pass + 1) * tap;
        const float want = std::pow(0.5f, static_cast<float>(pass));
        CHECK(wet_l[at] == doctest::Approx(want).epsilon(1e-4));
    }
    CHECK(*std::max_element(wet_l.begin(),
                            wet_l.begin() + static_cast<std::ptrdiff_t>(tap)) == 0.0f);

    // The tail has to *stop*. Decaying to 1e-38 and then feeding itself as a denormal forever is a
    // CPU leak and a hiss, and the only honest test of it is that the line is exactly empty.
    std::vector<float> silent(30000, 0.0f);
    delay.process(silent.data(), silent.data(), wet_l.data(), wet_r.data(),
                  static_cast<int>(silent.size()));
    CHECK(delay.silent());
}

TEST_CASE("delay: the knobs clamp, and a rate change cannot leave samples in the line") {
    Delay delay;
    delay.prepare(kRate);
    delay.set_feedback(2.0f);
    CHECK(delay.feedback() == doctest::Approx(0.95)); // 1.0 is an oscillator, not an effect
    delay.set_feedback(-1.0f);
    CHECK(delay.feedback() == 0.0f);
    delay.set_delay_seconds(99.0);
    CHECK(delay.delay_frames() == delay.size_frames() - 1u);
    delay.set_delay_seconds(-5.0);
    CHECK(delay.delay_frames() == 0u);

    // A line is samples. A delay line is samples.
    delay.prepare(96000.0);
    CHECK(delay.size_frames() == 48001u); // 0.5 s at 96 kHz: the line is samples, not seconds
    CHECK(delay.silent());
}

TEST_CASE("the bus with nothing sent to it is not in the signal path at all") {
    const int frames = 300;
    std::vector<float> dry_l(static_cast<std::size_t>(frames));
    std::vector<float> dry_r(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        dry_l[static_cast<std::size_t>(i)] = std::sin(static_cast<float>(i) * 0.05f);
        dry_r[static_cast<std::size_t>(i)] = std::cos(static_cast<float>(i) * 0.07f);
    }
    const std::vector<float> was_l = dry_l;
    const std::vector<float> was_r = dry_r;

    FxBus bus;
    bus.prepare(kRate);
    Params params; // every FX send defaults to zero
    CHECK_FALSE(FxBus::active(params));
    bus.process_block(dry_l.data(), dry_r.data(), frames, params);

    // Not "close to": bit-identical. This is the whole reason the committed goldens were allowed to
    // stay where they were, and a multiply by zero instead of a bypass would have passed "close".
    CHECK(dry_l == was_l);
    CHECK(dry_r == was_r);
}

TEST_CASE("the bus is invisible at defaults and audible where no note is playing") {
    const Params off = fx_params(); // mix 0: the shipping default
    Params on = off;
    on.fx_delay_mix = 0.4f; // the only difference in the world between these two engines

    Engine a;
    a.prepare(kRate);
    a.set_params(off);
    Engine again;
    again.prepare(kRate);
    again.set_params(off);
    Engine wet;
    wet.prepare(kRate);
    wet.set_params(on);
    arm(a);
    arm(again);
    arm(wet);

    const FxBuffer dry = render(a, 1);
    const FxBuffer twin = render(again, 1);
    const FxBuffer echo = render(wet, 1);

    // (a) The claim the goldens rest on: with every send at zero, the bus is not in the path. Not
    // "close to", and not measured against a second engine's audio either - against a second engine
    // with the same settings, so a bit-for-bit match means the bus added nothing rather than adding
    // the same thing twice.
    CHECK(dry.left == twin.left);
    CHECK(dry.right == twin.right);

    // (b) Between the note and its echo there is a window where nothing should be hearable at all,
    // and after it a window where the echo is the only thing left: the note died 150 ms ago.
    const std::size_t between = kSettleFrames + static_cast<std::size_t>(0.15 * kRate);
    const std::size_t between_end = kSettleFrames + static_cast<std::size_t>(0.2 * kRate);
    const std::size_t echo_at = kSettleFrames + static_cast<std::size_t>(0.226 * kRate);
    const std::size_t echo_end = kSettleFrames + static_cast<std::size_t>(0.27 * kRate);

    CHECK(window_peak(dry.left, between, between_end) < 1e-5f);
    CHECK(window_peak(echo.left, between, between_end) < 1e-5f);
    CHECK(window_peak(dry.left, echo_at, echo_end) < 1e-5f);
    // The point of the test: an effect that reads the right parameters and never reaches the output
    // is a slider that moves. This is the echo arriving, on its own, after the note is over.
    CHECK(window_peak(echo.left, echo_at, echo_end) > 1e-3f);
}

TEST_CASE("the echo is the dry render plus the send times itself, and it is inside the fader") {
    // -40 dB is where the master clipper is a straight line (test_note_trail.cpp measured 0.028 dB
    // of error there), so the sum the bus performs is checkable against the dry render instead of
    // against a description of it.
    Params base = fx_params();
    base.fx_delay_time_ms = 120.0f;
    base.fx_delay_mix = 0.5f;
    base.master_gain_db = -40.0f;
    // 120 ms at 48 kHz is exactly 5760 frames, which is what lets the equation below be checked
    // against a delayed copy of the dry render instead of against a description of it.
    const std::size_t n = 5760;
    const std::size_t d = kSettleFrames + n; // the echo starts one delay after the note
    Params reference_params = base;
    reference_params.fx_delay_mix = 0.0f;
    Engine reference;
    reference.prepare(kRate);
    reference.set_params(reference_params);
    Engine echoed;
    echoed.prepare(kRate);
    echoed.set_params(base);
    arm(reference);
    arm(echoed);

    const FxBuffer ref = render(reference, 1);
    const FxBuffer out = render(echoed, 1);

    // The wet output is the dry output plus a delayed copy of the dry output: the master gain factors
    // out because it is the same number in both engines, and the clipper is linear at this level. A
    // few frames past the block boundary, because the tap rounds the delay time to a whole frame -
    // which is the only error here once the parameter slew has settled (see kSettleFrames).
    float worst = 0.0f;
    const float scale = max_abs(ref.left);
    REQUIRE(scale > 0.0f);
    for (std::size_t i = d + 8; i < d + 4000; ++i) {
        const float want = ref.left[i] + 0.5f * ref.left[i - n];
        worst = std::max(worst, std::fabs(want - out.left[i]));
    }
    INFO("worst error " << worst << " against a dry peak of " << scale);
    CHECK(worst < 2e-4f * scale);

    // And the tail is inside the fader, not alongside it, because the bus sits before the gain: 20 dB
    // down on the master is 20 dB down on the echo, measured in a window where only the echo is left.
    Params quiet = base;
    quiet.master_gain_db = -60.0f; // the floor of the row, and exactly 20 dB below the render above
    Engine faded;
    faded.prepare(kRate);
    faded.set_params(quiet);
    arm(faded);
    const FxBuffer low = render(faded, 1);

    const std::size_t from = d + 200;
    const std::size_t to = d + 2200;
    const float loud_peak = window_peak(out.left, from, to);
    const float faded_peak = window_peak(low.left, from, to);
    INFO("echo peak " << loud_peak << " against the same echo 20 dB down: " << faded_peak);
    REQUIRE(loud_peak > 1e-4f);
    CHECK(faded_peak == doctest::Approx(loud_peak / 10.0f).epsilon(0.02f));
}

TEST_CASE("the three sends are rows like every other parameter, with the defaults that mean off") {
    const Params defaults;
    CHECK(defaults.fx_delay_mix == 0.0f);     // a piano that arrives already echoing is a patch
    CHECK(defaults.fx_delay_feedback < 1.0f); // 1.0 is an oscillator
    CHECK(defaults.fx_delay_time_ms <= 500.0f);

    // The table owns the range, on every path a parameter arrives from.
    Params params;
    params.fx_delay_mix = 100.0f;
    params.fx_delay_feedback = 4.0f;
    params.clamp_all();
    CHECK(params.fx_delay_mix == doctest::Approx(1.0));
    CHECK(params.fx_delay_feedback == doctest::Approx(0.95));

    // A NaN parameter poisons every sample it touches, so it becomes the row's default instead.
    Params broken;
    broken.fx_delay_mix = std::nanf("");
    broken.sanitize();
    CHECK(broken.fx_delay_mix == 0.0f);

    // And the bus agrees with the table about what "off" is.
    CHECK_FALSE(FxBus::active(defaults));
    Params on;
    on.fx_delay_mix = 0.001f;
    CHECK(FxBus::active(on));
}
