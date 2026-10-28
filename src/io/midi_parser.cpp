#include "boreaz/io/midi_parser.h"

namespace boreaz::io {

using engine::Event;

std::uint8_t MidiParser::message_length(std::uint8_t status) {
    if (status < 0x80)
        return 0; // a data byte is never a message start
    if (status >= 0xF8)
        return 1; // realtime, interleaves anywhere
    switch (status & 0xF0) {
    case 0x80:
    case 0x90:
    case 0xA0:
    case 0xB0:
    case 0xE0:
        return 3;
    case 0xC0:
    case 0xD0:
        return 2;
    default:
        break;
    }
    // System common.
    switch (status) {
    case 0xF1:
    case 0xF3:
        return 2;
    case 0xF2:
        return 3;
    default:
        return 1; // 0xF0, 0xF7, 0xF4, 0xF5, 0xF6
    }
}

std::optional<Event> MidiParser::feed(std::uint8_t byte) {
    ++bytes_in_;

    // Realtime (0xF8-0xFF) may arrive between any two bytes, including inside a dump. It is a
    // clock, a sense-of-life, or a machine stop: none of them is a note, and none of them may
    // disturb a message in progress.
    if (byte >= 0xF8)
        return std::nullopt;

    // 0xF0 inside a dump means the device restarted without terminating the previous one, so
    // that byte falls through and starts a new dump below.
    if (in_sysex_ && byte != 0xF0) {
        if (byte == 0xF7)
            in_sysex_ = false;
        else
            ++sysex_bytes_;
        return std::nullopt;
    }

    if (byte >= 0x80) {
        const std::uint8_t length = message_length(byte);
        status_ = byte;
        want_ = static_cast<std::uint8_t>(length - 1u);
        have_ = 0;
        if (byte < 0xF0)
            running_ = byte; // running status applies to channel messages only
        else if (byte == 0xF0)
            in_sysex_ = true;
        if (want_ != 0)
            return std::nullopt;
        // A one-byte message of its own: nothing here acts on it.
        status_ = 0;
        return std::nullopt;
    }

    // A data byte. With no message in progress it belongs to the last channel status.
    if (status_ == 0) {
        if (running_ == 0) {
            ++bad_bytes_; // a data byte out of nowhere
            return std::nullopt;
        }
        status_ = running_;
        want_ = static_cast<std::uint8_t>(message_length(running_) - 1u);
        have_ = 0;
    }

    if (have_ < 2)
        pending_[have_] = byte;
    ++have_;
    if (have_ < want_)
        return std::nullopt;

    const std::uint8_t type = status_ & 0xF0;
    const std::uint8_t channel = status_ & 0x0F;
    const std::uint8_t one = pending_[0];
    const std::uint8_t two = pending_[1];
    status_ = 0;

    switch (type) {
    case 0x80:
        ++events_out_;
        return Event::note_off(tick_, channel, one);
    case 0x90:
        // Velocity 0 is a note off on the wire. It stays a note_on with velocity 0 so that the
        // one place deciding what that means is the engine.
        ++events_out_;
        return Event::note_on(tick_, channel, one, two);
    case 0xB0:
        ++events_out_;
        return Event::control(tick_, channel, one, two);
    case 0xE0:
        ++events_out_;
        return Event::bend(tick_, channel, static_cast<std::int16_t>(one | (two << 7)));
    default:
        // Aftertouch, program change, channel pressure, and every system common message: read
        // for length, then dropped, because the engine has nothing to do with them yet.
        return std::nullopt;
    }
}

std::optional<Event> MidiParser::feed_packed(std::uint32_t packed) {
    // Windows packs status in the lowest byte, then up to two data bytes. The high byte is the
    // timestamp of a *later* message in the same WinMM delivery, not a data byte, so feeding
    // four bytes here would corrupt the next note.
    const std::uint8_t status = static_cast<std::uint8_t>(packed & 0xFFu);
    const std::uint32_t length = message_length(status);
    std::optional<Event> out;
    for (std::uint32_t i = 0; i < length; ++i) {
        const auto byte = static_cast<std::uint8_t>((packed >> (8u * i)) & 0xFFu);
        if (const auto event = feed(byte))
            out = event;
    }
    return out;
}

} // namespace boreaz::io
