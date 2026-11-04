#pragma once

#include <cstdint>

#include "boreaz/dsp/tuning.h"

namespace boreaz::engine {

// Everything the audio thread may read about a note. MIDI never reaches a voice: note
// numbers are already transposed by the time they get here, so a voice cannot disagree with
// the keyboard about what it is playing.
struct NoteRequest {
    std::uint8_t channel = 0;
    int note = 60;         // MIDI note after transpose and octave shift, 0-127
    int key = 60;          // the key that produced it, before transposition
    float velocity = 0.8f; // 0..1
    float frequency = boreaz::dsp::Tuning::kDefaultReferenceHz;
    // The channel's bend at the moment the note started, in semitones. An oscillator needs none of
    // this - its pitch *is* `frequency` - but a recording's pitch comes from the file and the key,
    // so a sample voice has to be told the bend separately or a note struck with the wheel down
    // arrives in tune with nothing.
    float bend_semitones = 0.0f;
    // Whether this is a key outside A0-C8 is not stored: ask Engine::is_extended_key(note), so
    // the display and the engine can never disagree about it.
};

} // namespace boreaz::engine
