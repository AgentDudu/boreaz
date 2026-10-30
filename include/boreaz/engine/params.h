#pragma once

#include <algorithm>
#include <cmath>

namespace boreaz::engine {

// Flat parameter block. The UI thread owns one, copies it on every change and swaps the copy
// in; the audio thread reloads it once per block and never writes it.
//
// Every field is clamped by `clamp_all`, because this struct arrives from a slider, a preset
// file, and a .mid, and none of those are allowed to make a filter or a gain go non-finite.
struct Params {
    float master_gain_db = -12.0f;
    int transpose_semitones = 0; // clamped to +/-24: beyond that the LUT leaves the keyboard
    int octave_shift = 0;        // clamped to +/-4

    // Concert pitch. The range is the orchestra's, 415-466 Hz, and `dsp::Tuning` refuses anything
    // outside it rather than building a lookup table of nonsense. Applied when a note starts:
    // `transpose` is the control that retunes what is already sounding.
    float tuning_reference_hz = 440.0f;

    // Oscillator mix. Osc 1 is the fundamental at unity, the rest are levels relative to it.
    // The default is a piano-ish comb: strong 2nd and 3rd partial, a little saw colour,
    // and a noise transient for the hammer.
    float osc2_level = 0.35f;
    float osc3_level = 0.12f;
    float osc_detune_cents = 4.0f;
    float noise_level = 0.12f;
    float noise_decay_s = 0.06f; // the hammer click is short; without this it is a hiss

    // Filter. Cutoff is proportional to the note, which is what a piano does: the bass is
    // dark and the treble is not. 1.0 would sit right on the fundamental and eat the harmonics.
    float filter_cutoff_ratio = 8.0f;
    // Svf range: 0 is no peak (Q = 0.5), 1 is close to self-oscillation (Q = 50).
    float filter_resonance = 0.35f;
    float filter_keytrack = 1.0f; // 0 = fixed cutoff, 1 = tracks the note fully

    // Amplitude envelope. A struck string is an attack and a long decay: sustain is the level
    // the string keeps ringing at while the key is down, so it is low, not 1.0.
    float amp_attack_s = 0.004f;
    float amp_decay_s = 2.0f;
    float amp_sustain = 0.18f;
    float amp_release_s = 0.6f;

    // Stereo spread across the 88 keys: -1 at A0, +1 at C8, scaled by this. 0 is mono.
    float pan_spread = 0.6f;

    // Pitch bend reach in semitones at full deflection.
    float bend_range_semitones = 2.0f;

    // Defined in `param_table.cpp`, over the descriptor table: the ranges are in one place, so a
    // parameter cannot be legal in a config file and out of range in the window. Called on every
    // path a parameter arrives from - a slider, a preset, a .mid - because none of them is allowed
    // to make a filter or a gain go non-finite.
    void clamp_all();

    // Anything non-finite is a bug upstream: a NaN parameter would poison every sample it touched,
    // so it becomes that row's default instead. Also defined over the table.
    void sanitize();

    // Two blocks are equal when the audio path would behave identically, which is what decides
    // whether a voice has to refresh its filter and envelope. Envelope *times* are excluded:
    // they only matter when a voice starts or is released, and a mid-note change to a release
    // time must not stop a note sounding (that rule belongs to phase 7's live parameter ramp).
    bool operator==(const Params& other) const {
        return master_gain_db == other.master_gain_db &&
               transpose_semitones == other.transpose_semitones &&
               octave_shift == other.octave_shift && osc2_level == other.osc2_level &&
               osc3_level == other.osc3_level && osc_detune_cents == other.osc_detune_cents &&
               noise_level == other.noise_level && noise_decay_s == other.noise_decay_s &&
               filter_cutoff_ratio == other.filter_cutoff_ratio &&
               filter_resonance == other.filter_resonance &&
               filter_keytrack == other.filter_keytrack && pan_spread == other.pan_spread &&
               bend_range_semitones == other.bend_range_semitones;
    }
    bool operator!=(const Params& other) const { return !(*this == other); }
};

} // namespace boreaz::engine
