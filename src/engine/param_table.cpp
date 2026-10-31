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

    // The FX bus. Every default is the effect off, which is not shyness: it is what lets the
    // committed goldens say the bus changed nothing, and it is what the row in `params.h` about "a
    // piano that arrives already echoing is a patch" means in practice.
    {"fx_delay_mix", "Delay mix", "", 0.0f, 1.0f, 0.0f, &Params::fx_delay_mix, nullptr},
    {"fx_delay_time_ms", "Delay time", " ms", 0.0f, 500.0f, 220.0f, &Params::fx_delay_time_ms,
     nullptr},
    {"fx_delay_feedback", "Delay feedback", "", 0.0f, 0.95f, 0.35f, &Params::fx_delay_feedback,
     nullptr},
};

} // namespace

// Defined here rather than in `params.h` so the ranges above are the only ranges there are.
void Params::clamp_all() {
    for (const ParamSpec& spec : kTable)
        param_write(spec, param_read(spec, *this), *this);
}

void Params::sanitize() {
    for (const ParamSpec& spec : kTable)
        if (spec.real != nullptr && !std::isfinite(this->*(spec.real)))
            this->*(spec.real) = spec.default_value; // clamp() would keep a NaN a NaN
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
    if (spec.real != nullptr)
        return params.*(spec.real);
    return static_cast<float>(params.*(spec.integer));
}

void param_write(const ParamSpec& spec, float value, Params& params) {
    if (!std::isfinite(value))
        value = spec.default_value; // a NaN parameter would poison every sample it touched
    if (spec.real != nullptr) {
        params.*(spec.real) = std::clamp(value, spec.minimum, spec.maximum);
        return;
    }
    // Rounded, not truncated: a slider that reports 512 for a value of 511.6 has lied about its own
    // position, and the file it writes then reads back as something else.
    params.*(spec.integer) =
        static_cast<int>(std::lround(std::clamp(value, spec.minimum, spec.maximum)));
}

void param_slew(Params& from, const Params& to, float alpha) {
    const float rate = std::clamp(alpha, 0.0f, 1.0f);
    for (const ParamSpec& spec : kTable) {
        if (spec.real == nullptr) {
            from.*(spec.integer) = to.*(spec.integer); // whole steps, whatever the block does
            continue;
        }
        const float here = from.*(spec.real);
        from.*(spec.real) = here + (to.*(spec.real) - here) * rate;
    }
}

} // namespace boreaz::engine
