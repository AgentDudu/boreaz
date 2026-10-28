#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "boreaz/engine/event.h"
#include "boreaz/io/midi_parser.h"

using boreaz::engine::Event;
using boreaz::engine::EventType;
using boreaz::io::MidiParser;

namespace {

// Feed a byte string, keep whatever events complete.
std::vector<Event> feed(MidiParser& parser, const std::vector<int>& bytes) {
    std::vector<Event> out;
    for (const int byte : bytes)
        if (const auto event = parser.feed(static_cast<std::uint8_t>(byte)))
            out.push_back(*event);
    return out;
}

} // namespace

TEST_CASE("midi parser: a note on is one event with its own channel, note and velocity") {
    MidiParser parser;
    parser.set_tick(1234);
    const auto events = feed(parser, {0x91, 0x3C, 0x7F});
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::kNoteOn);
    CHECK(events[0].channel == 1);
    CHECK(events[0].note == 60);
    CHECK(events[0].velocity == 127);
    CHECK(events[0].tick == 1234);
    CHECK(parser.events_out() == 1);
    CHECK(parser.bad_bytes() == 0);
}

TEST_CASE("midi parser: velocity 0 stays a note on with velocity 0") {
    // The wire says "note off"; deciding that is the engine's job, not the parser's, so there is
    // exactly one place where the rule lives.
    MidiParser parser;
    const auto events = feed(parser, {0x90, 0x40, 0x00});
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::kNoteOn);
    CHECK(events[0].velocity == 0);
}

TEST_CASE("midi parser: running status means a stream of notes needs one status byte") {
    MidiParser parser;
    const auto events = feed(parser, {0x90, 0x3C, 0x7F, 0x3D, 0x7F, 0x3E, 0x10});
    REQUIRE(events.size() == 3);
    CHECK(events[1].note == 61);
    CHECK(events[2].note == 62);
    CHECK(events[2].velocity == 16);
    CHECK(events[2].channel == 0);
}

TEST_CASE("midi parser: control change and 14 bit pitch bend") {
    MidiParser parser;
    const auto events = feed(parser, {0xB0, 0x40, 0x7F, 0xE2, 0x00, 0x40, 0xE2, 0x00, 0x00});
    REQUIRE(events.size() == 3);
    CHECK(events[0].type == EventType::kControlChange);
    CHECK(events[0].note == 64); // the sustain pedal, which is what CC 0x40 is
    CHECK(events[0].velocity == 127);
    CHECK(events[1].type == EventType::kPitchBend);
    CHECK(events[1].channel == 2);
    CHECK(events[1].value == 8192);
    CHECK(events[1].bend_normalized() == doctest::Approx(0.0f));
    CHECK(events[2].value == 0);
    CHECK(events[2].bend_normalized() == doctest::Approx(-1.0f));
}

TEST_CASE("midi parser: realtime messages interleave without breaking a note") {
    // Active sensing and the clock can land between any two bytes of a note. Dropping them must
    // not cost the note.
    MidiParser parser;
    const auto events = feed(parser, {0x90, 0xFE, 0xF8, 0x3C, 0xFE, 0x7F});
    REQUIRE(events.size() == 1);
    CHECK(events[0].note == 60);
    CHECK(events[0].velocity == 127);
    CHECK(parser.bad_bytes() == 0);
}

TEST_CASE("midi parser: a patch dump is skipped and the note after it still plays") {
    MidiParser parser;
    const auto events = feed(parser, {0xF0, 0x43, 0x12, 0x00, 0xF7, 0x90, 0x3C, 0x7F});
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::kNoteOn);
    CHECK(parser.sysex_bytes() == 3);
}

TEST_CASE("midi parser: a dump with no end is dropped when the next dump starts") {
    // Real devices do this after a power-on. The bytes in between must not become notes, and the
    // parser must come back clean.
    MidiParser parser;
    const auto during = feed(parser, {0xF0, 0x01, 0xF0, 0x02, 0xF7});
    CHECK(during.empty());
    CHECK(parser.sysex_bytes() == 2);
    const auto after = feed(parser, {0x90, 0x3C, 0x7F});
    REQUIRE(after.size() == 1);
    CHECK(after[0].note == 60);
}

TEST_CASE("midi parser: a stray data byte is counted, never guessed at") {
    MidiParser parser;
    const auto events = feed(parser, {0x40});
    CHECK(events.empty());
    CHECK(parser.bad_bytes() == 1);
}

TEST_CASE("midi parser: running status follows the last channel message, even a program change") {
    // Spec, not taste: program change is a channel message, so it owns running status afterwards.
    // A device mid-program-change therefore cannot be mistaken for a stream of notes.
    MidiParser parser;
    const auto events = feed(parser, {0x90, 0x3C, 0x7F, 0xC0, 0x05, 0x3D, 0x7F});
    REQUIRE(events.size() == 1);
    CHECK(events[0].note == 60);
    CHECK(parser.bad_bytes() == 0); // the two trailing pairs were program changes, not garbage
}

TEST_CASE("midi parser: a system common message leaves running status alone") {
    // 0xF1 time code is two bytes and is not a channel message, so the note running status after
    // it is still a note.
    MidiParser parser;
    const auto events = feed(parser, {0x90, 0x3C, 0x7F, 0xF1, 0x08, 0x3D, 0x7F});
    REQUIRE(events.size() == 2);
    CHECK(events[1].note == 61);
    CHECK(parser.bad_bytes() == 0);
}

TEST_CASE("midi parser: note off, and aftertouch read for length and dropped") {
    MidiParser parser;
    const auto events = feed(parser, {0x82, 0x3C, 0x40, 0xA0, 0x3C, 0x40});
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::kNoteOff);
    CHECK(events[0].channel == 2);
    CHECK(parser.bad_bytes() == 0);
}

TEST_CASE("midi parser: a packed winmm message gives it exactly its own bytes") {
    MidiParser parser;
    const auto events = parser.feed_packed(0x007F3C90u);
    REQUIRE(events.has_value());
    CHECK(events->note == 60);
    CHECK(parser.bytes_in() == 3);
}

TEST_CASE("midi parser: the high byte of a packed message is a timestamp, not data") {
    // Windows puts up to three message bytes in dwParam1 and a timestamp in the fourth for some
    // messages. Feeding four bytes here is how one short message corrupts the next.
    MidiParser parser;
    const auto event = parser.feed_packed(0x553412F3u); // song select: status plus one data byte
    CHECK_FALSE(event.has_value());
    CHECK(parser.bytes_in() == 2);
    CHECK(parser.bad_bytes() == 0);
}

