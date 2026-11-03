#include "boreaz/engine/param_table.h"

#include <algorithm>
#include <cmath>

namespace boreaz::engine {
namespace {

// Ranges and defaults live here and nowhere else. `Params` starts at the same numbers, and a test
// checks that the two agree, because a default that differs between the struct and the table is a
// parameter that is one `Reset` button away from a different instrument.
constexpr ParamSpec kTable[] = {
    // Volume and where the keyboard sits.
    {"master_gain_db", "Master", " dB", -72.0f, 12.0f, -12.0f, &Params::master_gain_db, nullptr},
    {"transpose_semitones", "Transpose", " st", -24.0f, 24.0f, 0.0f, nullptr,
     &Params::transpose_semitones},
    {"octave_shift", "Octave", " oct", -4.0f, 4.0f, 0.0f, nullptr, &Params::octave_shift},
    {"tuning_reference_hz", "Tuning A4", " Hz", 415.0f, 466.0f, 440.0f,
     &Params::tuning_reference_hz, nullptr},

    // The filter, which is proportional to the note because that is what a piano does.
    {"filter_cutoff_ratio", "Cutoff", " x f", 0.05f, 24.0f, 8.0f, &Params::filter_cutoff_ratio,
     nullptr},
    {"filter_resonance", "Resonance", "", 0.0f, 1.0f, 0.35f, &Params::filter_resonance, nullptr},
    {"filter_keytrack", "Key track", "", 0.0f, 1.0f, 1.0f, &Params::filter_keytrack, nullptr},

    // Oscillator mix: osc 1 is the fundamental, the rest are levels relative to it.
    {"osc2_level", "Osc 2", "", 0.0f, 2.0f, 0.35f, &Params::osc2_level, nullptr},
    {"osc3_level", "Osc 3", "", 0.0f, 2.0f, 0.12f, &Params::osc3_level, nullptr},
    {"osc_detune_cents", "Detune", " ct", -100.0f, 100.0f, 4.0f, &Params::osc_detune_cents,
     nullptr},
    {"noise_level", "Hammer noise", "", 0.0f, 1.0f, 0.12f, &Params::noise_level, nullptr},
    {"noise_decay_s", "Noise decay", " s", 0.001f, 2.0f, 0.06f, &Params::noise_decay_s, nullptr},

    // Amplitude envelope: a struck string is an attack and a long decay.
    {"amp_attack_s", "Attack", " s", 0.0005f, 10.0f, 0.004f, &Params::amp_attack_s, nullptr},
    {"amp_decay_s", "Decay", " s", 0.001f, 60.0f, 2.0f, &Params::amp_decay_s, nullptr},
    {"amp_sustain", "Sustain", "", 0.0f, 1.0f, 0.18f, &Params::amp_sustain, nullptr},
    {"amp_release_s", "Release", " s", 0.005f, 60.0f, 0.6f, &Params::amp_release_s, nullptr},

    // Stereo and the pitch wheel.
    {"pan_spread", "Stereo spread", "", 0.0f, 1.0f, 0.6f, &Params::pan_spread, nullptr},
    {"bend_range_semitones", "Bend range", " st", 0.0f, 24.0f, 2.0f, &Params::bend_range_semitones,
     nullptr},

    // The second envelope and the two LFOs. They are sources rather than sounds: nothing in the
    // unpatched instrument listens to them, so they reach the audio only through a slot below.
    {"env2_attack_s", "Env2 attack", " s", 0.0005f, 10.0f, 0.002f, &Params::env2_attack_s, nullptr},
    {"env2_decay_s", "Env2 decay", " s", 0.001f, 60.0f, 0.8f, &Params::env2_decay_s, nullptr},
    {"env2_sustain", "Env2 sustain", "", 0.0f, 1.0f, 0.0f, &Params::env2_sustain, nullptr},
    {"env2_release_s", "Env2 release", " s", 0.005f, 60.0f, 0.4f, &Params::env2_release_s, nullptr},
    {"lfo1_rate_hz", "LFO 1 rate", " Hz", 0.01f, 40.0f, 5.5f, &Params::lfo1_rate_hz, nullptr},
    {"lfo2_rate_hz", "LFO 2 rate", " Hz", 0.01f, 40.0f, 0.8f, &Params::lfo2_rate_hz, nullptr},

    // The eight modulation slots: four rows apiece, for what feeds the slot, what it moves, how
    // far, and whether it swings both ways or only up. Source and destination are whole numbers
    // because they are `ModSource` and `ModDest` indices, the same numbers a patch file writes.
    // Every slot starts switched off with no depth, which is the promise the FX sends make too: an
    // unpatched instrument pays nothing, and the goldens get to say so. `slot` is the array index,
    // so these rows and `SynthVoice`'s loop agree by construction about which slot is which.
    {"mod1_source", "M1 src", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModSource, 0},
    {"mod1_dest", "M1 dst", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModDest, 0},
    {"mod1_depth", "M1 depth", "", -1.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModDepth, 0},
    {"mod1_unipolar", "M1 uni", "", 0.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModUnipolar,
     0},
    {"mod2_source", "M2 src", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModSource, 1},
    {"mod2_dest", "M2 dst", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModDest, 1},
    {"mod2_depth", "M2 depth", "", -1.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModDepth, 1},
    {"mod2_unipolar", "M2 uni", "", 0.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModUnipolar,
     1},
    {"mod3_source", "M3 src", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModSource, 2},
    {"mod3_dest", "M3 dst", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModDest, 2},
    {"mod3_depth", "M3 depth", "", -1.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModDepth, 2},
    {"mod3_unipolar", "M3 uni", "", 0.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModUnipolar,
     2},
    {"mod4_source", "M4 src", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModSource, 3},
    {"mod4_dest", "M4 dst", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModDest, 3},
    {"mod4_depth", "M4 depth", "", -1.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModDepth, 3},
    {"mod4_unipolar", "M4 uni", "", 0.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModUnipolar,
     3},
    {"mod5_source", "M5 src", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModSource, 4},
    {"mod5_dest", "M5 dst", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModDest, 4},
    {"mod5_depth", "M5 depth", "", -1.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModDepth, 4},
    {"mod5_unipolar", "M5 uni", "", 0.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModUnipolar,
     4},
    {"mod6_source", "M6 src", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModSource, 5},
    {"mod6_dest", "M6 dst", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModDest, 5},
    {"mod6_depth", "M6 depth", "", -1.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModDepth, 5},
    {"mod6_unipolar", "M6 uni", "", 0.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModUnipolar,
     5},
    {"mod7_source", "M7 src", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModSource, 6},
    {"mod7_dest", "M7 dst", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModDest, 6},
    {"mod7_depth", "M7 depth", "", -1.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModDepth, 6},
    {"mod7_unipolar", "M7 uni", "", 0.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModUnipolar,
     6},
    {"mod8_source", "M8 src", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModSource, 7},
    {"mod8_dest", "M8 dst", "", 0.0f, 6.0f, 0.0f, nullptr, nullptr, ParamArray::kModDest, 7},
    {"mod8_depth", "M8 depth", "", -1.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModDepth, 7},
    {"mod8_unipolar", "M8 uni", "", 0.0f, 1.0f, 0.0f, nullptr, nullptr, ParamArray::kModUnipolar,
     7},

    // The FX bus. Every default is the effect off, which is not shyness: it is what lets the
    // committed goldens say the bus changed nothing, and it is what the row in `params.h` about "a
    // piano that arrives already echoing is a patch" means in practice.
    {"fx_distortion_mix", "Drive mix", "", 0.0f, 1.0f, 0.0f, &Params::fx_distortion_mix, nullptr},
    {"fx_distortion_drive", "Drive", "x", 1.0f, 10.0f, 3.0f, &Params::fx_distortion_drive, nullptr},
    {"fx_delay_mix", "Delay mix", "", 0.0f, 1.0f, 0.0f, &Params::fx_delay_mix, nullptr},
    {"fx_delay_time_ms", "Delay time", " ms", 0.0f, 500.0f, 220.0f, &Params::fx_delay_time_ms,
     nullptr},
    {"fx_delay_feedback", "Delay feedback", "", 0.0f, 0.95f, 0.35f, &Params::fx_delay_feedback,
     nullptr},
    {"fx_reverb_mix", "Room mix", "", 0.0f, 1.0f, 0.0f, &Params::fx_reverb_mix, nullptr},
    {"fx_reverb_seconds", "Room size", " s", 0.15f, 12.0f, 1.8f, &Params::fx_reverb_seconds,
     nullptr},
};

// Which element of an array a row means. `slot` is written by the table above and is in range by
// construction, so the only conversion needed here is the unsigned one the arrays ask for.
std::size_t slot_of(const ParamSpec& spec) noexcept {
    return static_cast<std::size_t>(spec.slot);
}

} // namespace

// The two switches below are the whole array mechanism: a row names an array of `Params` and an
// index into it, and every path that reads or writes a parameter goes through them, so a slot
// cannot be clamped in one place and left unclamped in another.

// Defined here rather than in `params.h` so the ranges above are the only ranges there are.
void Params::clamp_all() {
    for (const ParamSpec& spec : kTable)
        param_write(spec, param_read(spec, *this), *this);
}

void Params::sanitize() {
    // Asked through the table rather than field by field, so a row that addresses an array is a row
    // like any other: a second mechanism for reaching one is how a parameter ends up unsanitised.
    // A whole-number row always reads back finite, so this only ever fires on a real one.
    for (const ParamSpec& spec : kTable)
        if (!std::isfinite(param_read(spec, *this)))
            param_write(spec, spec.default_value, *this); // clamp() would keep a NaN a NaN
    clamp_all();
}

const ParamSpec* param_table(std::size_t* count) {
    if (count != nullptr)
        *count = std::size(kTable);
    return kTable;
}

const ParamSpec* param_spec(std::string_view key) {
    for (const ParamSpec& spec : kTable)
        if (key == spec.key)
            return &spec;
    return nullptr;
}

float param_read(const ParamSpec& spec, const Params& params) {
    switch (spec.array_kind) {
        case ParamArray::kModSource:
            return static_cast<float>(params.mod_source[slot_of(spec)]);
        case ParamArray::kModDest:
            return static_cast<float>(params.mod_dest[slot_of(spec)]);
        case ParamArray::kModDepth:
            return params.mod_depth[slot_of(spec)];
        case ParamArray::kModUnipolar:
            return static_cast<float>(params.mod_unipolar[slot_of(spec)]);
        case ParamArray::kNone:
            break;
    }
    if (spec.real != nullptr)
        return params.*(spec.real);
    return static_cast<float>(params.*(spec.integer));
}

void param_write(const ParamSpec& spec, float value, Params& params) {
    if (!std::isfinite(value))
        value = spec.default_value; // a NaN parameter would poison every sample it touched
    // Clamped for the array rows by the same rule as a scalar: a slider, a text file and a MIDI
    // mapping are not allowed to disagree about which slot numbers are legal.
    const float clamped = std::clamp(value, spec.minimum, spec.maximum);
    switch (spec.array_kind) {
        case ParamArray::kModSource:
            params.mod_source[slot_of(spec)] = static_cast<int>(std::lround(clamped));
            return;
        case ParamArray::kModDest:
            params.mod_dest[slot_of(spec)] = static_cast<int>(std::lround(clamped));
            return;
        case ParamArray::kModDepth:
            params.mod_depth[slot_of(spec)] = clamped;
            return;
        case ParamArray::kModUnipolar:
            params.mod_unipolar[slot_of(spec)] = static_cast<int>(std::lround(clamped));
            return;
        case ParamArray::kNone:
            break;
    }
    if (spec.real != nullptr) {
        params.*(spec.real) = clamped;
        return;
    }
    // Rounded, not truncated: a slider that reports 512 for a value of 511.6 has lied about its own
    // position, and the file it writes then reads back as something else.
    params.*(spec.integer) = static_cast<int>(std::lround(clamped));
}

void param_slew(Params& from, const Params& to, float alpha) {
    const float rate = std::clamp(alpha, 0.0f, 1.0f);
    for (const ParamSpec& spec : kTable) {
        // Read and written through the table, so a slot depth is smoothed exactly like a knob and
        // a slot's source or destination moves in whole steps exactly like a transpose.
        const bool continuous = spec.real != nullptr || spec.array_kind == ParamArray::kModDepth;
        if (!continuous) {
            param_write(spec, param_read(spec, to), from); // whole steps, whatever the block does
            continue;
        }
        const float here = param_read(spec, from);
        param_write(spec, here + (param_read(spec, to) - here) * rate, from);
    }
}

} // namespace boreaz::engine
