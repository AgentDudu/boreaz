// The FX bus: the delay, the room, and the bus that holds them.
//
// The delay came first because it is the effect whose every number can be asserted: an impulse
// comes out `n` samples later, scaled by exactly the send, and the tail reaches exact zero instead
// of trailing into denormals. The room is the one this instrument actually needs - a recorded grand
// played dry is a MIDI file - and it is asserted the way a room can be: where it starts, how fast
// it falls, how far under the note a full send sits, and that it is the same linear map when the
// input is halved. What stays unasserted is whether it sounds like a hall, which is an ear's job.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "boreaz/dsp/delay.h"
#include "boreaz/dsp/reverb.h"
#include "boreaz/engine/engine.h"
#include "boreaz/engine/event.h"
#include "boreaz/engine/fx_bus.h"

namespace {

using boreaz::dsp::Reverb;
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

// Energy per sample in [from, to). A room is a decay, and a decay is a sequence of levels rather
// than a peak, so the windows have to be measured as a quantity that survives the ringing.
float window_rms(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = from; i < std::min(to, samples.size()); ++i) {
        sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
        ++n;
    }
    return n > 0 ? static_cast<float>(std::sqrt(sum / static_cast<double>(n))) : 0.0f;
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
    REQUIRE(engine.push_event(Event::note_on(static_cast<std::uint32_t>(kSettleFrames), 0,
                                             std::uint8_t(69), std::uint8_t(110))));
    REQUIRE(engine.push_event(
        Event::note_off(static_cast<std::uint32_t>(kSettleFrames + 2400), 0, std::uint8_t(69))));
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
    delay.process(in.data(), silent.data(), wet_l.data(), wet_r.data(),
                  static_cast<int>(in.size()));

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
    CHECK(*std::max_element(wet_l.begin(), wet_l.begin() + static_cast<std::ptrdiff_t>(tap)) ==
          0.0f);

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

    // The wet output is the dry output plus a delayed copy of the dry output: the master gain
    // factors out because it is the same number in both engines, and the clipper is linear at this
    // level. A few frames past the block boundary, because the tap rounds the delay time to a whole
    // frame - which is the only error here once the parameter slew has settled (see kSettleFrames).
    float worst = 0.0f;
    const float scale = max_abs(ref.left);
    REQUIRE(scale > 0.0f);
    for (std::size_t i = d + 8; i < d + 4000; ++i) {
        const float want = ref.left[i] + 0.5f * ref.left[i - n];
        worst = std::max(worst, std::fabs(want - out.left[i]));
    }
    INFO("worst error " << worst << " against a dry peak of " << scale);
    CHECK(worst < 2e-4f * scale);

    // And the tail is inside the fader, not alongside it, because the bus sits before the gain: 20
    // dB down on the master is 20 dB down on the echo, measured in a window where only the echo is
    // left.
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

TEST_CASE("every send is a row like every other parameter, with the defaults that mean off") {
    const Params defaults;
    CHECK(defaults.fx_delay_mix == 0.0f);     // a piano that arrives already echoing is a patch
    CHECK(defaults.fx_delay_feedback < 1.0f); // 1.0 is an oscillator
    CHECK(defaults.fx_delay_time_ms <= 500.0f);
    CHECK(defaults.fx_reverb_mix == 0.0f);
    CHECK(defaults.fx_reverb_seconds > 0.0f);
    CHECK(defaults.fx_reverb_seconds < 12.0f); // the ceiling the room itself refuses

    // The table owns the range, on every path a parameter arrives from.
    Params params;
    params.fx_delay_mix = 100.0f;
    params.fx_delay_feedback = 4.0f;
    params.fx_reverb_mix = 40.0f;
    params.fx_reverb_seconds = -3.0f;
    params.clamp_all();
    CHECK(params.fx_delay_mix == doctest::Approx(1.0));
    CHECK(params.fx_delay_feedback == doctest::Approx(0.95));
    CHECK(params.fx_reverb_mix == doctest::Approx(1.0));
    CHECK(params.fx_reverb_seconds == doctest::Approx(0.15)); // the floor, not a negative room

    // A NaN parameter poisons every sample it touches, so it becomes the row's default instead.
    Params broken;
    broken.fx_delay_mix = std::nanf("");
    broken.fx_reverb_seconds = std::nanf("");
    broken.sanitize();
    CHECK(broken.fx_delay_mix == 0.0f);
    CHECK(broken.fx_reverb_seconds == doctest::Approx(1.8));

    // And the bus agrees with the table about what "off" is, for either effect.
    CHECK_FALSE(FxBus::active(defaults));
    Params echo;
    echo.fx_delay_mix = 0.001f;
    CHECK(FxBus::active(echo));
    Params room;
    room.fx_reverb_mix = 0.001f;
    CHECK(FxBus::active(room));
}

TEST_CASE("reverb: a room answers 25 ms late, falls at the size it was told, and the ears differ") {
    Reverb room;
    room.prepare(kRate);
    room.set_room_seconds(1.8);

    std::vector<float> in(static_cast<std::size_t>(3 * kRate), 0.0f);
    in[100] = 1.0f;
    std::vector<float> wet_l(in.size(), 0.0f);
    std::vector<float> wet_r(in.size(), 0.0f);
    room.process(in.data(), in.data(), wet_l.data(), wet_r.data(), static_cast<int>(in.size()));

    // The shortest comb is the room's aperture: 1116 frames at 44.1 kHz, 1215 here, one lap of the
    // shortest line. A "reverb" that answers on the sample the note lands on is a phaser.
    const std::size_t aperture = 1215;
    CHECK(window_peak(wet_l, 0, 100 + aperture) == 0.0f);
    CHECK(wet_l[100 + aperture] != 0.0f);

    // And it falls, and keeps falling: each 250 ms window is quieter than the one before it. A peak
    // cannot carry this claim - a comb output is a spike train, so peak and rms disagree by orders
    // of magnitude - but energy per sample does.
    for (std::size_t w = 0; w < 5; ++w) {
        const std::size_t from = 100 + w * 12000;
        const float before = window_rms(wet_l, from, from + 12000);
        const float after = window_rms(wet_l, from + 12000, from + 24000);
        INFO("window " << w << ": " << before << " then " << after);
        CHECK(after < before * 0.75f);
    }

    // One source, two ears: the same input on both sides and the channels are still not the same
    // signal, which is the whole stereo this effect has.
    double diff = 0.0;
    double magnitude = 0.0;
    for (std::size_t i = 100 + aperture; i < wet_l.size(); ++i) {
        diff += std::fabs(static_cast<double>(wet_l[i]) - static_cast<double>(wet_r[i]));
        magnitude += std::fabs(static_cast<double>(wet_l[i]));
    }
    INFO("mean |L-R| over mean |L| = " << diff / magnitude);
    CHECK(diff > 0.1 * magnitude);

    // The tail stops, and nothing along the way is a number the master cannot survive.
    CHECK(window_rms(wet_l, static_cast<std::size_t>(2.5 * kRate), wet_l.size()) < 1e-4f);
    for (const float v : wet_l)
        REQUIRE(std::isfinite(v));
}

TEST_CASE(
    "reverb: a full send is 13.5 dB under the note, exactly linear, and cannot ring forever") {
    // A held fortissimo, the level a piano actually reaches: 0.4 amplitude A3 for two seconds, long
    // enough for a 1.8 s room to fill up.
    const std::size_t n = static_cast<std::size_t>(4 * kRate);
    std::vector<float> note(n, 0.0f);
    for (std::size_t i = 0; i < n / 2; ++i)
        note[i] = 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 220.0 *
                                                     static_cast<double>(i) / kRate));

    Reverb room;
    room.prepare(kRate);
    room.set_room_seconds(1.8);
    std::vector<float> wet_l(n, 0.0f);
    std::vector<float> wet_r(n, 0.0f);
    room.process(note.data(), note.data(), wet_l.data(), wet_r.data(), static_cast<int>(n));

    // The number that says `fx_reverb_mix` is a send and not a make-up gain. Measured as energy,
    // because a comb output is a spike train and its peak is 3 dB more generous than its rms: one
    // second into a held note, a full send is 13.5 dB under the note feeding it. A room any louder
    // than that would be a second piano; quieter than -16 dB and the mix knob does nothing a player
    // can hear.
    const std::size_t filled = static_cast<std::size_t>(kRate);
    const float note_rms = window_rms(note, filled, filled + 24000); // 0.4 sine: 0.283
    const float room_rms = window_rms(wet_l, filled, filled + 24000);
    const float send_db = 20.0f * std::log10(room_rms / note_rms);
    INFO("full send at 1.8 s: " << room_rms << " against a note of " << note_rms << " = " << send_db
                                << " dB");
    CHECK(send_db > -16.0f);
    CHECK(send_db < -10.0f);
    // And it is a tail rather than a second voice: the peak 0.5-1.5 s after the key is let go is
    // down about 34 dB, bounded here at 30 dB, which is heard as a room and not as a note that will
    // not stop.
    INFO("tail peak: " << window_peak(wet_l, n / 2 + 24000, n / 2 + 72000));
    CHECK(window_peak(wet_l, n / 2 + 24000, n / 2 + 72000) < 0.4f * 0.032f);

    // Linear to the last bit, which is what lets the mix mean a send: halve the input and every
    // sample of the room halves.
    Reverb quieter;
    quieter.prepare(kRate);
    quieter.set_room_seconds(1.8);
    std::vector<float> half_note = note;
    for (float& v : half_note)
        v *= 0.5f;
    std::vector<float> half_l(n, 0.0f);
    std::vector<float> half_r(n, 0.0f);
    quieter.process(half_note.data(), half_note.data(), half_l.data(), half_r.data(),
                    static_cast<int>(n));
    float worst = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(2.0f * half_l[i] - wet_l[i]));
    INFO("worst |2 * half - full| = " << worst);
    CHECK(worst < 1e-6f * note_rms);

    // The longest room the table allows, hit with the hardest near-DC wave a parameter file can ask
    // for. This is the headroom bound the master has to survive at a full send, and the reason the
    // comb loops have a leak: 0.90 without one, 0.14 with it.
    Reverb hall;
    hall.prepare(kRate);
    hall.set_room_seconds(12.0);
    std::vector<float> hard(static_cast<std::size_t>(12 * kRate), 0.0f);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kRate); i += 2)
        hard[i] = 0.5f;
    std::vector<float> hall_l(hard.size(), 0.0f);
    std::vector<float> hall_r(hard.size(), 0.0f);
    hall.process(hard.data(), hard.data(), hall_l.data(), hall_r.data(),
                 static_cast<int>(hard.size()));
    INFO("12 s room, hardest input available: peak " << max_abs(hall_l));
    CHECK(max_abs(hall_l) < 0.2f);
    for (const float v : hall_l)
        REQUIRE(std::isfinite(v));
    CHECK(window_rms(hall_l, static_cast<std::size_t>(11 * kRate), hall_l.size()) < 1e-2f);
}

TEST_CASE("reverb: the size knob clamps to rooms that exist, and a rate change leaves no history") {
    Reverb room;
    room.prepare(kRate);
    room.set_room_seconds(-1.0);
    CHECK(room.room_seconds() == doctest::Approx(0.15)); // the floor, not a negative room
    room.set_room_seconds(1.0e9);
    CHECK(room.room_seconds() == doctest::Approx(12.0));
    // The ceiling shows up where it matters, in the loop gain: below 0.995 per lap whatever the
    // seconds said, which is a room, not an oscillator with a knob on it.
    CHECK(room.feedback() <= 0.995f);
    CHECK(room.feedback() > 0.0f);

    const std::size_t at_48k = room.max_line_frames();
    std::vector<float> in(2000, 0.0f);
    in[10] = 1.0f;
    std::vector<float> wet_l(2000, -1.0f);
    std::vector<float> wet_r(2000, -1.0f);
    room.process(in.data(), in.data(), wet_l.data(), wet_r.data(), 2000);
    CHECK(max_abs(wet_l) > 0.0f);

    // A line measured in samples means nothing at a different rate, so the history goes.
    room.prepare(96000.0);
    CHECK(room.all_silent());
    CHECK(room.max_line_frames() > at_48k);

    // Unprepared, an effect is silence rather than the caller's own buffer handed back.
    Reverb cold;
    std::fill(wet_l.begin(), wet_l.end(), -1.0f);
    std::fill(wet_r.begin(), wet_r.end(), -1.0f);
    cold.process(in.data(), in.data(), wet_l.data(), wet_r.data(), 2000);
    CHECK(max_abs(wet_l) == 0.0f);
    CHECK(max_abs(wet_r) == 0.0f);
}

TEST_CASE("the room outlives the note it came from and fades with the piano") {
    Params base = fx_params();
    base.fx_reverb_mix = 0.6f;
    base.fx_reverb_seconds = 2.0f;
    base.master_gain_db = -12.0f; // where the app ships
    Params dry = base;
    dry.fx_reverb_mix = 0.0f;

    Engine bare_engine;
    bare_engine.prepare(kRate);
    bare_engine.set_params(dry);
    Engine roomy;
    roomy.prepare(kRate);
    roomy.set_params(base);
    arm(bare_engine);
    arm(roomy);
    const FxBuffer bare = render(bare_engine, 2);
    const FxBuffer wet = render(roomy, 2);

    // A second and a bit after a 50 ms note died: nothing is playing, so whatever is here is the
    // room, and the same window in the dry engine is the proof that it is not the note.
    const std::size_t from = kSettleFrames + 48000;
    const std::size_t to = from + 24000;
    INFO("dry " << window_peak(bare.left, from, to) << " against wet "
                << window_peak(wet.left, from, to));
    CHECK(window_peak(bare.left, from, to) < 1e-5f);
    CHECK(window_peak(wet.left, from, to) > 1e-4f);

    // Inside the fader rather than beside it, measured on the tail alone: 20 dB down on the master
    // is 20 dB down on the room.
    Params pulled = base;
    pulled.master_gain_db = -32.0f;
    Engine faded;
    faded.prepare(kRate);
    faded.set_params(pulled);
    arm(faded);
    const FxBuffer low = render(faded, 2);
    const float loud = window_peak(wet.left, from, to);
    const float soft = window_peak(low.left, from, to);
    INFO("tail " << loud << " against the same tail 20 dB down: " << soft);
    CHECK(soft == doctest::Approx(loud / 10.0f).epsilon(0.05f));
}

TEST_CASE("reverb: the size knob is a room, measured as a decay rather than as a coefficient") {
    // One second of A4 and then silence, so the tail is the only thing left to measure. The decay
    // is read off successive 500 ms windows and the fastest pair wins, because the earliest windows
    // still hold the room filling up and would flatter the number.
    const auto decay_db_per_second = [&](double seconds) {
        Reverb room;
        room.prepare(kRate);
        room.set_room_seconds(seconds);
        const std::size_t n = static_cast<std::size_t>((2.0 + 3.0 * seconds) * kRate);
        std::vector<float> in(n, 0.0f);
        for (std::size_t i = 0; i < static_cast<std::size_t>(kRate); ++i)
            in[i] = 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 440.0 *
                                                       static_cast<double>(i) / kRate));
        std::vector<float> wet_l(n, 0.0f);
        std::vector<float> wet_r(n, 0.0f);
        room.process(in.data(), in.data(), wet_l.data(), wet_r.data(), static_cast<int>(n));

        double fastest = 0.0;
        for (double t = 1.25; t + 1.0 < 2.0 + 3.0 * seconds; t += 0.25) {
            const std::size_t a = static_cast<std::size_t>(t * kRate);
            const double before = window_rms(wet_l, a, a + 24000);
            const double after = window_rms(wet_l, a + 24000, a + 48000);
            if (before > 1e-9 && after > 1e-9)
                fastest = std::max(fastest, -20.0 * std::log10(after / before) * 2.0);
        }
        return fastest;
    };

    const double small = decay_db_per_second(1.8);
    const double large = decay_db_per_second(12.0);
    INFO("1.8 s room: " << small << " dB/s (RT60 about " << 60.0 / small << " s), 12 s room: "
                        << large << " dB/s (RT60 about " << 60.0 / large << " s)");
    // The knob does what it says, in order and to within the honesty band the header quotes: asking
    // for 1.8 s measures as 1.6 s because the damping and the leak each take a little off every
    // lap.
    CHECK(small > 0.0);
    CHECK(large > 0.0);
    CHECK(large < small / 3.0);
    CHECK(60.0 / small > 1.2);
    CHECK(60.0 / small < 2.4);
    CHECK(60.0 / large > 5.0);
}
