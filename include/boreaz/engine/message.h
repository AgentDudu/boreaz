#pragma once

#include <cstdint>
#include <optional>

#include "boreaz/engine/event.h"

namespace boreaz::engine {

// A channel voice message, unpacked: the status byte and up to two data bytes. Nothing about it
// says where it came from, which is the point - a byte off a keyboard, a byte off a MIDI clock,
// and a byte out of a file are the same message and have to mean the same thing.
struct MidiMessage {
    std::uint8_t status = 0;
    std::uint8_t first = 0;
    std::uint8_t second = 0;

    std::uint8_t channel() const { return status & 0x0FU; }
    std::uint8_t kind() const { return status & 0xF0U; }
};

// One message, what the engine does with it, and nothing else. Returns no event for a message the
// engine cannot use yet, and for anything that is not a channel voice message; saying which ones
// were dropped, and to whom, belongs to the caller, because only the caller knows whether the
// bytes came from a file, a keyboard, or a cable that is talking to itself.
//
// Three rules live here and nowhere else, and they are the rules a second implementation gets
// wrong:
//   - "note on, velocity zero" is passed through as a note on. The engine decides that it ends a
//     note (see Engine::accept), because a keyboard and a file must agree on it.
//   - a note off's second byte is a *release* velocity, not a strike velocity, so it is dropped
//     rather than used to pick a sample.
//   - pitch bend keeps all 14 bits, because 8192 means "wheel centred" and rounding it to 8 bits
//     would put the centre a quarter-tone out.
std::optional<Event> decode_message(const MidiMessage& message, std::int64_t position);

} // namespace boreaz::engine
