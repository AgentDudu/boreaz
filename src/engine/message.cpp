#include "boreaz/engine/message.h"

namespace boreaz::engine {

std::optional<Event> decode_message(const MidiMessage& message, std::int64_t position) {
    const std::uint8_t channel = message.channel();
    const std::uint8_t first = message.first;
    const std::uint8_t second = message.second;

    switch (message.kind()) {
        case 0x80: // note off. Its velocity is how hard the key was released, which we do not use.
            return Event::note_off(position, channel, first);
        case 0x90:
            // Velocity zero stays a note on with velocity zero on purpose: see the header.
            return Event::note_on(position, channel, first, second);
        case 0xB0:
            return Event::control(position, channel, first, second);
        case 0xE0: {
            // 14 bits, LSB first. The cast is safe because both bytes are masked to 7 bits by the
            // sender; a file that ignores that gets a wheel a bit further over, not a wrong note.
            const std::int32_t value = static_cast<std::int32_t>(first) |
                                      (static_cast<std::int32_t>(second) << 7);
            return Event::bend(position, channel, static_cast<std::int16_t>(value & 0x3FFF));
        }
        default:
            // Note off with a strike velocity (0xA0 polyphonic aftertouch), program change (0xC0),
            // channel pressure (0xD0), and every system message: the engine has nothing for them
            // yet, so they arrive, are understood, and stop here.
            return std::nullopt;
    }
}

} // namespace boreaz::engine
