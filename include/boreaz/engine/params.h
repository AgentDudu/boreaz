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

    // The FX bus: sends, not crossfades. The dry mix is never attenuated to make room for the
    // effect, so a send of zero is the dry signal untouched and a send of one is the dry signal
    // plus a copy of itself - which is why `fx_delay_mix = 0` is a bypass the goldens can rely on.
    // The default is silence, because a piano that arrives already echoing is a patch, not an
    // instrument.
    float fx_delay_mix = 0.0f;
    float fx_delay_time_ms = 220.0f;
    // 0.95 at the top, not 1.0: a feedback of one is an oscillator, and no parameter file gets to
    // build one. The clamp is the same one the delay line applies to anything that bypasses the
    // table.
    float fx_delay_feedback = 0.35f;
    // The room. Size is RT60 - the seconds it takes to fall 60 dB - rather than a bare feedback
    // number, because 0.95 against 0.98 is a cathedral and nothing in a parameter file means that
    // in words. A full send puts the room 13.5 dB under a held note, which is a room you can hear
    // rather than a second piano; the 0.6 a patch would reach is 3 dB under that again. Measured in
    // tests/test_fx.cpp, which also owns the fact that the size knob under-delivers by about 11 %.
    float fx_reverb_mix = 0.0f;
    float fx_reverb_seconds = 1.8f;

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
        // The FX sends are deliberately not here. They are read by the bus every block, so a delay
        // knob changes the sound on the block it lands on and needs no voice to be re-prepared -
        // and an effect parameter that counts here would refresh every voice on every knob move.
    }
    bool operator!=(const Params& other) const { return !(*this == other); }
};

} // namespace boreaz::engine
