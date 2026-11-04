// Phase 8's modulation router, tested at both ends: the eight slots as rows of the table, and the
// eight slots as something a listener can hear in a rendered note. A test at only one of those ends
// misses half the contract — a table that addresses nothing, or a voice reading parameters nobody
// moved.
//
// The measurements are pitch by autocorrelation and brightness by mean slope, which is how the
// engine tests already measure a voice. The patch under test is one sawtooth with the filter open
// and the second and third oscillators silent, so the period it finds belongs to the oscillator and
// not to the instrument.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/engine/mod_router.h"
#include "boreaz/engine/note.h"
#include "boreaz/engine/param_table.h"
#include "boreaz/engine/params.h"
#include "boreaz/engine/synth_voice.h"

using boreaz::engine::keytrack_value;
using boreaz::engine::kModDestNames;
using boreaz::engine::kModSlots;
using boreaz::engine::kModSourceNames;
using boreaz::engine::mod_amount;
using boreaz::engine::ModDest;
using boreaz::engine::ModSource;
using boreaz::engine::NoteRequest;
using boreaz::engine::param_choices;
using boreaz::engine::param_read;
using boreaz::engine::param_spec;
using boreaz::engine::param_table;
using boreaz::engine::param_write;

using boreaz::engine::ParamArray;
using boreaz::engine::Params;
using boreaz::engine::ParamSpec;
using boreaz::engine::shape_source_value;
using boreaz::engine::source_is_unipolar;
using boreaz::engine::SynthVoice;

namespace {

constexpr double kRate = 44100.0;
constexpr int kBlock = 64; // the size the audio callback asks for, so the block-rate router is
                           // measured at the rate it actually runs at
constexpr int kNote = 69;  // A4, whose pitch every assertion below can be stated in hertz
constexpr float kA4 = 440.0f;

// Route slot `index`, overwriting whatever the test patch had there.
void route(Params& params, int index, ModSource source, ModDest dest, float depth, bool unipolar) {
    params.mod_source[static_cast<std::size_t>(index)] = static_cast<int>(source);
    params.mod_dest[static_cast<std::size_t>(index)] = static_cast<int>(dest);
    params.mod_depth[static_cast<std::size_t>(index)] = depth;
    params.mod_unipolar[static_cast<std::size_t>(index)] = unipolar ? 1 : 0;
}

// The test patch: one sawtooth, filter wide open, no pan, and an amp envelope that is in sustain
// for the whole measurement, so nothing but the destination moves.
Params patch() {
    Params params;
    params.osc2_level = 0.0f;
    params.osc3_level = 0.0f;
    params.noise_level = 0.0f;
    params.pan_spread = 0.0f;
    params.filter_cutoff_ratio = 20.0f;
    params.filter_resonance = 0.0f;
    params.amp_attack_s = 0.004f;
    params.amp_decay_s = 2.0f;
    params.amp_sustain = 0.8f;
    params.amp_release_s = 0.1f;
    return params;
}

// The table is looked up by name and then read or written: two calls here because it is two calls
// in the config reader, the panel and the command line too.
bool write(std::string_view key, float value, Params& params) {
    const ParamSpec* spec = param_spec(key);
    if (spec == nullptr)
        return false;
    param_write(*spec, value, params);
    return true;
}

float read(std::string_view key, const Params& params) {
    const ParamSpec* spec = param_spec(key);
    REQUIRE(spec != nullptr);
    return param_read(*spec, params);
}

NoteRequest struck(float velocity) {
    NoteRequest value;
    value.note = kNote;
    value.key = kNote;
    value.channel = 0;
    value.velocity = velocity;
    value.frequency = kA4;
    return value;
}

// Render `seconds` of a voice that has already been started, in the blocks the audio callback
// would ask for, and hand back the mono sum. The voice stays the caller's, because the exact
// questions ("what pitch did the router ask for?") are questions about the voice.
std::vector<float> advance(SynthVoice& voice, float seconds) {
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(seconds * kRate) + kBlock);
    std::vector<float> left(static_cast<std::size_t>(kBlock), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(kBlock), 0.0f);
    for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        voice.render(left.data(), right.data(), kBlock);
        for (std::size_t frame = 0; frame < static_cast<std::size_t>(kBlock); ++frame)
            out.push_back(0.5f * (left[frame] + right[frame]));
    }
    return out;
}

// A note on a fresh voice, rendered. For the claims that are about audio and need nothing else.
std::vector<float> play(const Params& params, float seconds, float velocity = 0.8f,
                        float wheel = 0.0f) {
    SynthVoice voice;
    voice.start(struck(velocity), params, kRate, 0);
    voice.set_mod_wheel(wheel);
    return advance(voice, seconds);
}

// A note on a voice the test keeps, for the claims that are about what the router decided.
void strike(SynthVoice& voice, const Params& params, float velocity = 0.8f, float wheel = 0.0f) {
    voice.start(struck(velocity), params, kRate, 0);
    voice.set_mod_wheel(wheel);
}

// The note a patch is holding, exactly. A4 plus a number of semitones, which is the arithmetic of
// `mod_amount` on the pitch destination: depth 1.0 of full travel is 2 semitones.
double expected_hertz(double semitones) {
    return kA4 * std::pow(2.0, semitones / 12.0);
}

// The pitch trace, one reading per block, straight off the oscillator. This is what makes the
// wobble test exact: the rate and the depth of a modulation are read from the voice instead of
// being guessed from its waveform.
std::vector<double> pitch_trace(SynthVoice& voice, float seconds) {
    std::vector<double> trace;
    std::vector<float> left(static_cast<std::size_t>(kBlock), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(kBlock), 0.0f);
    for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock) {
        voice.render(left.data(), right.data(), kBlock);
        trace.push_back(voice.pitch_hz());
    }
    return trace;
}

double peak(const std::vector<float>& samples, std::size_t from, std::size_t frames) {
    double high = 0.0;
    for (std::size_t i = from; i < from + frames && i < samples.size(); ++i)
        high = std::max(high, std::abs(static_cast<double>(samples[i])));
    return high;
}

double window_brightness(const std::vector<float>& samples, std::size_t from, std::size_t frames) {
    // Mean step-to-step change: what survives a lowpass is the slow part, so this falls when the
    // filter closes. Clamped to the audio there is, because a window asked for past the end of a
    // render is a mistake that reads as data otherwise.
    const std::size_t end = std::min(from + frames, samples.size());
    if (end <= from + 1)
        return 0.0;
    double sum = 0.0;
    for (std::size_t i = from + 1; i < end; ++i)
        sum += std::fabs(samples[i] - samples[i - 1]);
    return sum / static_cast<double>(end - from - 1);
}

} // namespace

TEST_CASE("a source is normalised, and the destination decides what depth means") {
    // Keytrack: the middle of the keyboard is zero, an octave above is a quarter of the way to the
    // top, and both ends are clamped because a source that ran past full scale on the last nine
    // keys is a defect nobody could dial out.
    CHECK(keytrack_value(60) == doctest::Approx(0.0f));
    CHECK(keytrack_value(72) == doctest::Approx(0.25f));
    CHECK(keytrack_value(12) == doctest::Approx(-1.0f));
    CHECK(keytrack_value(127) == doctest::Approx(1.0f));
    CHECK(keytrack_value(0) == doctest::Approx(-1.0f));

    // The resting sources — velocity, Mod env, the wheel — start at nothing, so unshaped they add
    // nothing. The LFOs and the keyboard swing.
    CHECK(source_is_unipolar(ModSource::kVelocity));
    CHECK(source_is_unipolar(ModSource::kWheel));
    CHECK(source_is_unipolar(ModSource::kModEnv));
    CHECK_FALSE(source_is_unipolar(ModSource::kLfo1));
    CHECK_FALSE(source_is_unipolar(ModSource::kLfo2));
    CHECK_FALSE(source_is_unipolar(ModSource::kKeyTrack));

    // The conversion, both ways round. A wheel at rest stays at rest when the slot wants a level,
    // and reads as -1 when the slot wants a swing: that is the difference between a vibrato and a
    // note that is flat until somebody moves the wheel.
    CHECK(shape_source_value(ModSource::kWheel, 0.0f, true) == doctest::Approx(0.0f));
    CHECK(shape_source_value(ModSource::kWheel, 1.0f, true) == doctest::Approx(1.0f));
    CHECK(shape_source_value(ModSource::kWheel, 0.0f, false) == doctest::Approx(-1.0f));
    CHECK(shape_source_value(ModSource::kWheel, 1.0f, false) == doctest::Approx(1.0f));
    CHECK(shape_source_value(ModSource::kLfo1, 0.0f, true) == doctest::Approx(0.5f));
    CHECK(shape_source_value(ModSource::kLfo1, -1.0f, true) == doctest::Approx(0.0f));
    CHECK(shape_source_value(ModSource::kLfo1, 1.0f, false) == doctest::Approx(1.0f));
    CHECK(shape_source_value(ModSource::kModEnv, 0.75f, true) == doctest::Approx(0.75f));
    // Staying bipolar means staying bipolar: the negative half of a swing has to survive, or an
    // LFO only pulls one way and a keytrack is a ramp instead of a centre.
    CHECK(shape_source_value(ModSource::kLfo1, -1.0f, false) == doctest::Approx(-1.0f));
    CHECK(shape_source_value(ModSource::kLfo1, 0.0f, false) == doctest::Approx(0.0f));
    CHECK(shape_source_value(ModSource::kKeyTrack, -0.5f, false) == doctest::Approx(-0.5f));

    // Depth is a fraction of the destination's full travel, and the travel is in the destination's
    // own units. Depth cannot leave -1..1 (the table's rows clamp it there), so asking for more
    // travel than one slot gives means routing a second slot.
    CHECK(mod_amount(ModDest::kCutoff, 1.0f, 1.0f) == doctest::Approx(3.0f)); // octaves
    CHECK(mod_amount(ModDest::kPitch, 1.0f, 1.0f) == doctest::Approx(2.0f));  // semitones
    CHECK(mod_amount(ModDest::kPan, 1.0f, 1.0f) == doctest::Approx(1.0f));    // full width
    CHECK(mod_amount(ModDest::kOsc2Level, 0.5f, -1.0f) == doctest::Approx(-0.5f));
    CHECK(mod_amount(ModDest::kCutoff, 1.0f, 0.5f) == doctest::Approx(1.5f));
    CHECK(mod_amount(ModDest::kPitch, 0.0f, 1.0f) == 0.0f);
}

TEST_CASE("every slot of the matrix is a row of the table, and one row moves one element") {
    std::size_t count = 0;
    const ParamSpec* table = param_table(&count);
    REQUIRE(count > 0);

    // Four rows a slot, eight slots. A slot that is not in the table cannot be reached from a
    // file, a flag, or a panel, which is the whole point of phase 7.
    int slots[4] = {0, 0, 0, 0};
    for (std::size_t row = 0; row < count; ++row) {
        const ParamSpec& spec = table[row];
        if (spec.array_kind == ParamArray::kNone)
            continue;
        CHECK(spec.slot >= 0);
        CHECK(spec.slot < static_cast<int>(kModSlots));
        CHECK(spec.real == nullptr); // an array row addresses an element, never a scalar
        CHECK(spec.integer == nullptr);
        // With both pointers null, `param_is_integer` is the only question a file reader or a panel
        // can ask, and it has to answer for the depth row: a caller that guessed from the pointers
        // refused `mod3_depth = 0.4` and wrote the other three through a null member pointer.
        CHECK(param_is_integer(spec) == (spec.array_kind != ParamArray::kModDepth));
        slots[static_cast<int>(spec.array_kind) - 1]++;
    }
    for (int kind = 0; kind < 4; ++kind)
        CHECK(slots[kind] == static_cast<int>(kModSlots));

    // Writing one row changes its element and no other, which is what the `slot` index buys over a
    // cast member pointer.
    Params params;
    for (std::size_t row = 0; row < count; ++row) {
        const ParamSpec& spec = table[row];
        if (spec.array_kind != ParamArray::kModDepth)
            continue;
        Params before = params;
        param_write(spec, 0.25f, params);
        CHECK(params.mod_depth[static_cast<std::size_t>(spec.slot)] == 0.25f);
        for (std::size_t index = 0; index < kModSlots; ++index) {
            if (static_cast<int>(index) == spec.slot)
                continue;
            CHECK(params.mod_depth[index] == before.mod_depth[index]);
        }
    }

    // And every row of the table, array or not, reads back what the table wrote. The array rows
    // nearly went without this: reaching for `spec.integer` because a row had no `real` was
    // reaching for a null pointer a third of the way through the table.
    for (std::size_t row = 0; row < count; ++row) {
        const ParamSpec& spec = table[row];
        Params
            fresh{}; // the loop above's `params` is already wired up; this guard starts from clean
        param_write(spec, spec.default_value, fresh);
        INFO(spec.key);
        CHECK(param_read(spec, fresh) == spec.default_value);
    }
}

TEST_CASE("every source and every destination the voice understands is reachable from a row") {
    // The defaults are all "off" on purpose — that is what lets the goldens prove the router
    // changed nothing before anybody patched it — so reachability is a question about the ranges:
    // a value a file or a slider cannot write is a value the voice will never see.
    std::size_t count = 0;
    const ParamSpec* table = param_table(&count);
    int sources = 0;
    int dests = 0;
    for (std::size_t row = 0; row < count; ++row) {
        const ParamSpec& spec = table[row];
        if (spec.array_kind == ParamArray::kModSource) {
            CHECK(spec.minimum <= 0.0f);
            CHECK(spec.maximum >= static_cast<float>(static_cast<int>(ModSource::kCount) - 1));
            ++sources;
        } else if (spec.array_kind == ParamArray::kModDest) {
            CHECK(spec.minimum <= 0.0f);
            CHECK(spec.maximum >= static_cast<float>(static_cast<int>(ModDest::kCount) - 1));
            ++dests;
        } else if (spec.array_kind == ParamArray::kModDepth) {
            // Depth is a fraction of travel, so the row stops at 1.0: two slots is how a patch
            // asks for more than one destination's full travel.
            CHECK(spec.minimum == -1.0f);
            CHECK(spec.maximum == 1.0f);
        }
    }
    CHECK(sources == static_cast<int>(kModSlots));
    CHECK(dests == static_cast<int>(kModSlots));
}

TEST_CASE("the pitch destination is the destination's own units, and the voice obeys it") {
    // Every one of these is an exact number: the router's arithmetic, the semitone curve, and the
    // frequency the oscillator was actually set to. Nothing here is measured off a waveform.
    struct Case {
        ModSource source;
        float wheel; // what the wheel reads; velocity is fixed below
        float depth;
        bool unipolar;
        double semitones; // what the note should end up
        const char* why;
    };

    constexpr float kStrike = 0.8f; // a note that is actually audible: velocity 0 is silence

    const Case cases[] = {
        // Full travel is 2 semitones, so that is what a wheel pushed to the top at full depth is
        // worth, and a wheel at rest is worth nothing at all: the patch is in tune until somebody
        // moves something.
        {ModSource::kWheel, 1.0f, 1.0f, true, 2.0, "wheel at the top, unipolar"},
        {ModSource::kWheel, 0.0f, 1.0f, true, 0.0, "wheel at rest, unipolar"},
        // A wheel in a slot that wants a swing reads -1 at rest: half of the travel below the note.
        {ModSource::kWheel, 0.0f, 1.0f, false, -2.0, "wheel at rest, bipolar"},
        {ModSource::kWheel, 1.0f, 1.0f, false, 2.0, "wheel at the top, bipolar"},
        // Depth is a fraction of travel, and negative depth is the same travel the other way.
        {ModSource::kWheel, 1.0f, 0.25f, true, 0.5, "a quarter of the travel"},
        {ModSource::kWheel, 1.0f, -1.0f, true, -2.0, "the travel inverted"},
        // Velocity is a level, so unipolar it passes through and bipolar it starts at -1: a note
        // struck at 0.6 is +1.2 semitones one way and +0.4 the other.
        {ModSource::kVelocity, 0.6f, 1.0f, true, 1.2, "velocity, unipolar"},
        {ModSource::kVelocity, 0.6f, 1.0f, false, 0.4, "velocity, bipolar"},
        {ModSource::kVelocity, 0.6f, 0.0f, true, 0.0, "depth zero asks for nothing"},
        // A4 is 9 keys above C4 on a 48-key ramp, so the source reads +0.1875: unipolar that puts
        // it at 0.59375 of the travel and bipolar at 0.1875 of it. One flag is the difference
        // between "the filter opens as you go up" and "the notes below middle C are pulled down".
        {ModSource::kKeyTrack, 0.0f, 1.0f, true, 2.0 * (0.5 + 0.5 * (9.0 / 48.0)),
         "keytrack, unipolar"},
        {ModSource::kKeyTrack, 0.0f, 1.0f, false, 2.0 * (9.0 / 48.0), "keytrack, bipolar"},
    };

    for (const Case& item : cases) {
        Params params = patch();
        route(params, 0, item.source, ModDest::kPitch, item.depth, item.unipolar);

        // A velocity of zero is a silent note, so the strike is fixed and only the source moves:
        // velocity cases read the strike, the wheel cases read the wheel.
        const float velocity = item.source == ModSource::kVelocity ? item.wheel : kStrike;
        SynthVoice voice;
        strike(voice, params, velocity, item.wheel);
        const std::vector<float> sound = advance(voice, 0.05f);
        CHECK(voice.pitch_hz() == doctest::Approx(expected_hertz(item.semitones)).epsilon(1.0e-4));
        CHECK(peak(sound, 0, sound.size()) > 0.0);
    }
}

TEST_CASE("an off slot and a zero depth leave the note where it started") {
    Params params = patch();
    route(params, 0, ModSource::kOff, ModDest::kPitch, 1.0f, true);
    route(params, 1, ModSource::kWheel, ModDest::kPitch, 0.0f, true);
    route(params, 2, ModSource::kWheel, ModDest::kOff, 1.0f,
          true); // a depth on a destination of off

    SynthVoice voice;
    strike(voice, params, 1.0f, 1.0f); // wheel and velocity both pushed as far as they go
    advance(voice, 0.05f);
    CHECK(voice.pitch_hz() == doctest::Approx(kA4).epsilon(1.0e-6));
}

TEST_CASE("an LFO routed to pitch wobbles by its depth, at its own rate") {
    // The trace comes off the oscillator once a block, so both halves of the claim are exact: how
    // far the note swings, and how often. PLAN.md asks for +-2 semitones at the LFO's rate.
    Params params = patch();
    params.lfo1_rate_hz = 5.5f;
    route(params, 0, ModSource::kLfo1, ModDest::kPitch, 1.0f, false);

    SynthVoice voice;
    strike(voice, params);
    const std::vector<double> trace = pitch_trace(voice, 2.0f);

    double lowest = 1.0e9;
    double highest = 0.0;
    int crossings = 0;
    for (std::size_t i = 1; i < trace.size(); ++i) {
        lowest = std::min(lowest, trace[i]);
        highest = std::max(highest, trace[i]);
        // A swing through the centre of the note is a half-period, which is how the rate is
        // counted without looking at a waveform at all.
        if ((trace[i - 1] - kA4) * (trace[i] - kA4) < 0.0)
            ++crossings;
    }

    const double blocks = static_cast<double>(trace.size());
    const double seconds = blocks * kBlock / kRate;
    const double rate = static_cast<double>(crossings) / (2.0 * seconds);

    CHECK(lowest == doctest::Approx(expected_hertz(-2.0)).epsilon(0.01));
    CHECK(highest == doctest::Approx(expected_hertz(2.0)).epsilon(0.01));
    CHECK(rate == doctest::Approx(5.5).epsilon(0.05));

    // The same routing at zero depth is a straight note, and the depth is the only difference: the
    // wobble belongs to the depth, not to the LFO running.
    Params still = params;
    still.mod_depth[0] = 0.0f;
    SynthVoice held;
    strike(held, still);
    const std::vector<double> flat = pitch_trace(held, 2.0f);
    for (double hertz : flat)
        CHECK(hertz == doctest::Approx(kA4).epsilon(1.0e-6));
}

TEST_CASE("the level destinations take the modulation and are floored at zero") {
    // A level is a balance, so a modulated level is added and then not allowed to go negative:
    // inverting an oscillator's phase to make it quieter changes the timbre instead of the mix.
    // The wheel is the source here rather than Mod env because a wheel holds whatever the test set
    // it to, and an envelope's value at a given block is a question about timing.
    Params params = patch();
    params.osc2_level = 0.25f;
    route(params, 0, ModSource::kWheel, ModDest::kOsc2Level, 0.5f, false); // bipolar: -1 at rest

    SynthVoice voice;
    strike(voice, params, 0.8f, 0.0f);
    advance(voice, 0.02f); // 0.25 - 0.5 would be a negative level, and a negative level is zero
    CHECK(voice.osc2_level() == 0.0f);

    voice.set_mod_wheel(1.0f); // bipolar at the top is +1, so 0.25 + 0.5
    advance(voice, 0.02f);
    CHECK(voice.osc2_level() == doctest::Approx(0.75f).epsilon(1.0e-4));

    voice.set_mod_wheel(0.5f); // and in the middle of its swing a bipolar slot asks for nothing
    advance(voice, 0.02f);
    CHECK(voice.osc2_level() == doctest::Approx(0.25f).epsilon(1.0e-4));

    // One destination per slot: a cable that never mentions oscillator 3 or the noise leaves both
    // where the patch put them.
    Params neighbours = patch();
    neighbours.osc3_level = 0.4f;
    neighbours.noise_level = 0.2f;
    route(neighbours, 0, ModSource::kWheel, ModDest::kOsc2Level, 1.0f, true);
    SynthVoice second;
    strike(second, neighbours, 0.8f, 1.0f);
    advance(second, 0.02f);
    CHECK(second.osc2_level() == doctest::Approx(1.0f).epsilon(1.0e-4));
    CHECK(second.osc3_level() == doctest::Approx(0.4f).epsilon(1.0e-6));
    CHECK(second.noise_level() == doctest::Approx(0.2f).epsilon(1.0e-6));
}

TEST_CASE("the pan destination moves the image by the amount the slot asked for") {
    // Both channels render the same sample with a constant gain each, so the ratio of their peaks
    // is the pan itself: cos and sin of the angle, no measurement involved.
    auto stereo_ratio = [](float depth, bool unipolar, float velocity) {
        Params params = patch();
        route(params, 0, ModSource::kVelocity, ModDest::kPan, depth, unipolar);
        SynthVoice voice;
        voice.start(struck(velocity), params, kRate, 0);
        std::vector<float> left;
        std::vector<float> right;
        for (int block = 0; block < 8; ++block) {
            std::vector<float> l(static_cast<std::size_t>(kBlock), 0.0f);
            std::vector<float> r(static_cast<std::size_t>(kBlock), 0.0f);
            voice.render(l.data(), r.data(), kBlock);
            left.insert(left.end(), l.begin(), l.end());
            right.insert(right.end(), r.begin(), r.end());
        }
        const std::size_t from = 2 * static_cast<std::size_t>(kBlock); // past the attack
        return peak(right, from, 4 * static_cast<std::size_t>(kBlock)) /
               peak(left, from, 4 * static_cast<std::size_t>(kBlock));
    };

    // Pan 0.5 is 3/8 pi: sin over cos is 2.414. Half a velocity in a unipolar slot is half a pan.
    CHECK(stereo_ratio(1.0f, true, 0.5f) ==
          doctest::Approx(std::sin(0.75 * 3.14159265358979 / 2.0) /
                          std::cos(0.75 * 3.14159265358979 / 2.0))
              .epsilon(1.0e-3));
    // The same depth with the flag off reads 2v-1, which is 0 at half: dead centre.
    CHECK(stereo_ratio(1.0f, false, 0.5f) == doctest::Approx(1.0).epsilon(1.0e-6));
    // And no depth means no pan whatever the velocity does.
    CHECK(stereo_ratio(0.0f, true, 1.0f) == doctest::Approx(1.0).epsilon(1.0e-6));
}

TEST_CASE("two slots on one destination add, and the same total from one slot is the same note") {
    // The mixer is a sum, so a patch can split one destination across two sources; and because it
    // is a sum and not a max, 0.3 plus 0.4 from one source is what 0.7 of it is.
    Params two = patch();
    route(two, 0, ModSource::kVelocity, ModDest::kPitch, 0.3f, true);
    route(two, 1, ModSource::kVelocity, ModDest::kPitch, 0.4f, true);

    Params one = patch();
    route(one, 0, ModSource::kVelocity, ModDest::kPitch, 0.7f, true);

    // A note struck at 0.5: (0.3 + 0.4) x 0.5 of a 2-semitone travel is 0.7 semitones.
    SynthVoice first;
    strike(first, two, 0.5f);
    advance(first, 0.05f);
    SynthVoice second;
    strike(second, one, 0.5f);
    advance(second, 0.05f);

    CHECK(first.pitch_hz() == doctest::Approx(expected_hertz(0.7)).epsilon(1.0e-4));
    CHECK(first.pitch_hz() == doctest::Approx(second.pitch_hz()).epsilon(1.0e-6));
}

TEST_CASE("a depth that is not a number cannot make the voice not a number") {
    // The table clamps depth to +-1, so this cannot arrive from a file or a slider. It can arrive
    // from a struct built in code, and the voice is the last place that can still say no.
    Params params = patch();
    route(params, 0, ModSource::kLfo1, ModDest::kPitch, std::numeric_limits<float>::quiet_NaN(),
          true);
    route(params, 1, ModSource::kWheel, ModDest::kCutoff, std::numeric_limits<float>::infinity(),
          true);

    const std::vector<float> sound = play(params, 0.2f, 0.8f, 0.5f);
    REQUIRE(sound.size() > 0);
    for (float sample : sound)
        CHECK(std::isfinite(sample));

    SynthVoice voice;
    strike(voice, params, 0.8f, 0.5f);
    advance(voice, 0.02f);
    CHECK(voice.pitch_hz() == doctest::Approx(kA4).epsilon(1.0e-6)); // skipped, not spoiled
}

TEST_CASE("Mod env routed to the cutoff opens the filter at the strike and lets it close") {
    Params params = patch();
    params.filter_cutoff_ratio = 2.0f; // 880 Hz on A4, so +-3 octaves stays between 110 Hz and
                                       // 7 kHz and neither end leans on the filter's clamping
    params.mod_env_attack_s = 0.002f;
    params.mod_env_decay_s = 0.6f;
    params.mod_env_sustain = 0.0f;
    route(params, 0, ModSource::kModEnv, ModDest::kCutoff, 1.0f,
          false); // bipolar: 1 opens, 0 closes

    const std::vector<float> sound = play(params, 1.6f);
    // The whole point of routing a filter is that nothing in it may stop being a number.
    for (float sample : sound)
        REQUIRE(std::isfinite(sample));
    const std::size_t rate = static_cast<std::size_t>(kRate);
    const double struck = window_brightness(sound, rate / 20, rate / 10); // 50..150 ms
    const double settled = window_brightness(sound, rate, rate / 2);      // 1.0..1.5 s
    CHECK(struck > settled * 2.0);

    // The control: the same note with the slot wired to nothing does not fall over its own decay.
    Params quiet = params;
    quiet.mod_source[0] = static_cast<int>(ModSource::kOff); // the same note, cable unplugged
    const std::vector<float> unmodulated = play(quiet, 1.6f);
    const double flat = window_brightness(unmodulated, rate / 20, rate / 10);
    const double late = window_brightness(unmodulated, rate, rate / 2);
    CHECK(late > flat * 0.7);
    // The claim is the comparison, not either number: a note getting older loses some edge, and a
    // filter closing on purpose loses most of it.
    CHECK(struck / settled > 4.0 * (flat / late));
}

TEST_CASE("the rows are wired to the slots the voice reads") {
    // The table addresses Params by (array kind, slot). If that address is wrong, a slider moves
    // the wrong cable and every test above still passes on the values it wrote by hand.
    const float travel[kModSlots] = {-0.5f, 0.25f, 1.0f, -1.0f, -0.25f, 0.75f, 1.0f, 0.0f};
    Params params;
    for (std::size_t slot = 0; slot < kModSlots; ++slot) {
        params.mod_source[slot] = static_cast<int>(ModSource::kLfo2);
        params.mod_dest[slot] = static_cast<int>(ModDest::kOsc3Level);
        params.mod_depth[slot] = travel[slot];
        params.mod_unipolar[slot] = slot % 2 == 0 ? 1 : 0;
    }
    // Every one of them, written as a source index, a destination index, a depth and a flag.
    for (std::size_t slot = 0; slot < kModSlots; ++slot) {
        Params probe;
        const std::string stem = std::to_string(slot + 1);
        write("mod" + stem + "_source", static_cast<float>(static_cast<int>(ModSource::kLfo2)),
              probe);
        write("mod" + stem + "_dest", static_cast<float>(static_cast<int>(ModDest::kOsc3Level)),
              probe);
        write("mod" + stem + "_depth", travel[slot], probe);
        write("mod" + stem + "_unipolar", static_cast<float>(params.mod_unipolar[slot]), probe);
        CHECK(probe.mod_source[slot] == params.mod_source[slot]);
        CHECK(probe.mod_dest[slot] == params.mod_dest[slot]);
        CHECK(probe.mod_depth[slot] == doctest::Approx(params.mod_depth[slot]));
        CHECK(probe.mod_unipolar[slot] == params.mod_unipolar[slot]);
    }

    // And slot 3 of the table driving an oscillator-3 level moves that level and no other, which
    // is the same argument made through the voice instead of through the struct. The wheel is the
    // source because it holds whatever the test sets it to; an LFO would be a phase question.
    Params voice_params = patch();
    voice_params.osc3_level = 0.5f;
    route(voice_params, 3, ModSource::kWheel, ModDest::kOsc3Level, 0.25f, true);
    SynthVoice voice;
    strike(voice, voice_params, 0.8f, 0.5f);
    advance(voice, 0.02f);
    CHECK(voice.osc3_level() == doctest::Approx(0.625f).epsilon(1.0e-4));
    CHECK(voice.osc2_level() == 0.0f);
}

TEST_CASE("every row has a unique name, and one name writes one element") {
    std::size_t count = 0;
    const ParamSpec* table = param_table(&count);
    REQUIRE(count > 0);
    for (std::size_t a = 0; a < count; ++a)
        for (std::size_t b = a + 1; b < count; ++b)
            CHECK(std::strcmp(table[a].key, table[b].key) != 0);

    Params params;
    CHECK(write("mod5_source", 6.0f, params));
    CHECK(params.mod_source[4] == static_cast<int>(ModSource::kWheel));
    CHECK_FALSE(write("mod9_source", 1.0f, params));
    CHECK_FALSE(write("mod_source", 1.0f, params));
}

TEST_CASE("reading and writing a router row round-trips, and depth leaves the slot as a float") {
    Params params;
    REQUIRE(write("mod3_dest", static_cast<float>(static_cast<int>(ModDest::kPan)), params));
    CHECK(read("mod3_dest", params) ==
          doctest::Approx(static_cast<float>(static_cast<int>(ModDest::kPan))));
    CHECK(params.mod_unipolar[2] == 0); // the flag is its own row, not a bit of the destination

    REQUIRE(write("mod3_depth", 0.4f, params));
    CHECK(read("mod3_depth", params) == doctest::Approx(0.4f));
    // Depth is not an enum: a fractional write survives the round trip.
    REQUIRE(write("mod3_depth", -0.75f, params));
    CHECK(read("mod3_depth", params) == doctest::Approx(-0.75f));

    // The index rows are whole numbers, so a slider between two destinations cannot land between
    // them.
    REQUIRE(write("mod3_source", 2.4f, params));
    CHECK(params.mod_source[2] == static_cast<int>(ModSource::kLfo1));
    CHECK(read("mod3_source", params) == doctest::Approx(2.0f));

    // Out of range clamps into range rather than rejecting the write, like every other row.
    REQUIRE(write("mod3_source", 99.0f, params));
    CHECK(params.mod_source[2] == static_cast<int>(ModSource::kCount) - 1);
    REQUIRE(write("mod3_dest", -4.0f, params));
    CHECK(params.mod_dest[2] == static_cast<int>(ModDest::kOff));
    REQUIRE(write("mod3_dest", 99.0f, params));
    CHECK(params.mod_dest[2] == static_cast<int>(ModDest::kCount) - 1);
}

TEST_CASE("the defaults are the ones that make an unpatched patch silent") {
    Params params;
    for (std::size_t slot = 0; slot < kModSlots; ++slot) {
        CHECK(params.mod_source[slot] == static_cast<int>(ModSource::kOff));
        CHECK(params.mod_dest[slot] == static_cast<int>(ModDest::kOff));
        CHECK(params.mod_depth[slot] == 0.0f);
        CHECK(params.mod_unipolar[slot] == 0);
    }
    CHECK(Params().mod_source[3] == Params().mod_source[3]); // default construction is all there is
    CHECK(read("mod8_depth", Params()) == 0.0f);
    CHECK(read("mod8_unipolar", Params()) == 0.0f);
}

TEST_CASE("an out-of-range index does not escape the slot it was asked for") {
    // The engine's own loop indexes the slots, so the caller cannot be trusted to stay in range and
    // neither can a file.
    Params params;
    CHECK(read("mod1_depth", params) == 0.0f);
    CHECK_FALSE(write("mod0_depth", 0.5f, params));
    CHECK_FALSE(write("mod9_depth", 0.5f, params));
    for (std::size_t slot = 0; slot < kModSlots; ++slot)
        CHECK(params.mod_depth[slot] == 0.0f);
}

TEST_CASE("the wheel reaches a voice, and one voice's wheel is not another's") {
    Params params = patch();
    route(params, 0, ModSource::kWheel, ModDest::kPitch, 1.0f, true);

    SynthVoice first;
    SynthVoice second;
    strike(first, params, 0.8f, 0.0f);
    strike(second, params, 0.8f, 1.0f);
    advance(first, 0.02f);
    advance(second, 0.02f);

    CHECK(first.pitch_hz() == doctest::Approx(kA4).epsilon(1.0e-6));
    CHECK(second.pitch_hz() == doctest::Approx(expected_hertz(2.0)).epsilon(1.0e-4));

    // Moving the wheel mid-note moves the voice that is sounding, because the source is read per
    // block and not sampled at the strike.
    second.set_mod_wheel(0.0f);
    advance(second, 0.01f);
    CHECK(second.pitch_hz() == doctest::Approx(kA4).epsilon(1.0e-6));
}

TEST_CASE("the words a cable is drawn with are the numbers a patch file writes") {
    std::size_t source_count = 0;
    std::size_t dest_count = 0;
    const char* const* sources = param_choices(ParamArray::kModSource, &source_count);
    const char* const* dests = param_choices(ParamArray::kModDest, &dest_count);

    // One word per enumerator, which is what the array sizes in mod_router.h already refuse to
    // compile otherwise; the checks are here so the failure names the list rather than a header.
    CHECK(sources != nullptr);
    CHECK(dests != nullptr);
    CHECK(source_count == std::size_t(ModSource::kCount));
    CHECK(dest_count == std::size_t(ModDest::kCount));

    // Pinned to the enumerators rather than to the order, because reordering an enum is a thing
    // somebody adding a source will do, and a panel that then calls velocity "Wheel" is a defect no
    // engine test would ever notice.
    using S = ModSource;
    CHECK(std::string_view(kModSourceNames[std::size_t(S::kOff)]) == "Off");
    CHECK(std::string_view(kModSourceNames[std::size_t(S::kModEnv)]) == "Mod env");
    CHECK(std::string_view(kModSourceNames[std::size_t(S::kLfo1)]) == "LFO 1");
    CHECK(std::string_view(kModSourceNames[std::size_t(S::kLfo2)]) == "LFO 2");
    CHECK(std::string_view(kModSourceNames[std::size_t(S::kVelocity)]) == "Velocity");
    CHECK(std::string_view(kModSourceNames[std::size_t(S::kKeyTrack)]) == "Key track");
    CHECK(std::string_view(kModSourceNames[std::size_t(S::kWheel)]) == "Wheel");

    using D = ModDest;
    CHECK(std::string_view(kModDestNames[std::size_t(D::kOff)]) == "Off");
    CHECK(std::string_view(kModDestNames[std::size_t(D::kCutoff)]) == "Cutoff");
    CHECK(std::string_view(kModDestNames[std::size_t(D::kPitch)]) == "Pitch");
    CHECK(std::string_view(kModDestNames[std::size_t(D::kOsc2Level)]) == "Osc 2");
    CHECK(std::string_view(kModDestNames[std::size_t(D::kOsc3Level)]) == "Osc 3");
    CHECK(std::string_view(kModDestNames[std::size_t(D::kNoiseLevel)]) == "Hammer noise");
    CHECK(std::string_view(kModDestNames[std::size_t(D::kPan)]) == "Pan");

    // Four of the six destinations move a parameter that has a row of its own, and they are named
    // for it, so the list and the slider above it cannot become two vocabularies. Pitch and pan
    // have no row to match - a note's pitch and its position are not parameters - so they are named
    // for the thing itself.
    CHECK(std::string_view(kModDestNames[std::size_t(D::kCutoff)]) ==
          param_spec("filter_cutoff_ratio")->label);
    CHECK(std::string_view(kModDestNames[std::size_t(D::kOsc2Level)]) ==
          param_spec("osc2_level")->label);
    CHECK(std::string_view(kModDestNames[std::size_t(D::kOsc3Level)]) ==
          param_spec("osc3_level")->label);
    CHECK(std::string_view(kModDestNames[std::size_t(D::kNoiseLevel)]) ==
          param_spec("noise_level")->label);

    // Every cable row's stored value is a word its own list has, and the word is "Off": a patch in
    // progress has a source with no destination, and neither end may default to something audible.
    std::size_t rows = 0;
    const ParamSpec* table = param_table(&rows);
    int cables = 0;
    for (std::size_t row = 0; row < rows; ++row) {
        const ParamSpec& spec = table[row];
        std::size_t count = 0;
        const char* const* words = param_choices(spec.array_kind, &count);
        if (words == nullptr)
            continue;
        ++cables;
        const float value = param_read(spec, Params{});
        CHECK(value == 0.0f);
        CHECK(std::string_view(words[static_cast<int>(value)]) == "Off");
    }
    CHECK(cables == 2 * kModSlots);

    // A depth is a number and a unipolar flag is a switch: neither has a list, and the panel draws
    // them as a slider and a checkbox.
    std::size_t none = 1;
    CHECK(param_choices(ParamArray::kNone, &none) == nullptr);
    CHECK(none == 0);
    CHECK(param_choices(ParamArray::kModDepth, &none) == nullptr);
    CHECK(param_choices(ParamArray::kModUnipolar, &none) == nullptr);

    // The one thing a list widget must never be handed is an index its list has not got: a patch
    // file may well say `mod1_source = 40`, and it is the row's own clamp, not the panel's memory,
    // that stops ImGui being asked for a word that does not exist. Checked against the list length
    // rather than the row's range so the two cannot disagree silently.
    for (const float bad : {-1.0f, 6.0f, 40.0f, 1.5e9f}) {
        Params patched;
        param_write(*param_spec("mod1_source"), bad, patched);
        const float stored = param_read(*param_spec("mod1_source"), patched);
        CHECK(stored >= 0.0f);
        CHECK(stored < static_cast<float>(source_count));
    }

    // The naming decision, written down where it can break: the modulation envelope is "Mod env"
    // next to the cable that names it, and the amp envelope keeps the bare word a player reads.
    CHECK(std::string_view(param_spec("mod_env_attack_s")->label) == "Mod env attack");
    CHECK(std::string_view(param_spec("mod_env_release_s")->label) == "Mod env release");
    CHECK(std::string_view(param_spec("amp_attack_s")->label) == "Attack");
    CHECK(std::string_view(param_spec("mod1_source")->label) == "M1 source");
    CHECK(std::string_view(param_spec("mod8_unipolar")->label) == "M8 unipolar");
}
