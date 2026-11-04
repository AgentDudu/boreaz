#pragma once

#include <cstdint>
#include <optional>

#include "boreaz/engine/event.h"

namespace boreaz::io {

// MIDI 1.0 byte stream -> engine events. Nothing here knows about WinMM, so the byte-level
// hazards (running status, realtime messages arriving mid-sysex, a status byte where a data
// byte belonged) are testable without a keyboard attached.
//
// Notes are not filtered to the 88-key span: an A0 on a keyboard and a note 21 from a .mid file
// are the same event, and PLAN 5 maps them to on-screen keys. A note outside the piano's range
// still sounds, which is what a synthesizer is supposed to do.
class MidiParser {
  public:
    // Events carry the tick last passed to set_tick(). The MIDI callback has no audio position
    // of its own, so the caller stamps it with the frames rendered so far.
    void set_tick(std::int64_t tick) { tick_ = tick; }
    std::int64_t tick() const { return tick_; }

    // One byte. Returns an event when a message completes.
    std::optional<boreaz::engine::Event> feed(std::uint8_t byte);

    // WinMM packs one short message into the low three bytes of dwParam1, status first.
    std::optional<boreaz::engine::Event> feed_packed(std::uint32_t packed);

    std::uint64_t bytes_in() const { return bytes_in_; }
    std::uint64_t events_out() const { return events_out_; }
    // Bytes that could not start or continue a message. A device talking some other protocol
    // shows up here rather than as wrong notes.
    std::uint64_t bad_bytes() const { return bad_bytes_; }
    // Bytes skipped inside a system-exclusive dump, which usually means "a patch dump arrived".
    std::uint64_t sysex_bytes() const { return sysex_bytes_; }

  private:
    // Length of a channel or common message, counting the status byte itself.
    static std::uint8_t message_length(std::uint8_t status);

    std::int64_t tick_ = 0;
    std::uint8_t status_ = 0;  // status of the message being assembled; 0 when none is
    std::uint8_t running_ = 0; // last channel status, which running status reuses
    std::uint8_t pending_[2] = {0, 0};
    std::uint8_t have_ = 0; // data bytes collected for the message in progress
    std::uint8_t want_ = 0; // data bytes expected for it
    bool in_sysex_ = false;

    std::uint64_t bytes_in_ = 0;
    std::uint64_t events_out_ = 0;
    std::uint64_t bad_bytes_ = 0;
    std::uint64_t sysex_bytes_ = 0;
};

} // namespace boreaz::io
