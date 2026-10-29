// What one channel voice message means is decided once, here, and both a live keyboard and a MIDI
// file go through this function. That makes it load-bearing in a way its size hides, so it is
// tested where it lives rather than through a file that happens to contain the bytes.
#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include "boreaz/engine/event.h"
#include "boreaz/engine/message.h"

using boreaz::engine::decode_message;
using boreaz::engine::Event;
using boreaz::engine::EventType;
using boreaz::engine::MidiMessage;

namespace {
constexpr std::int64_t kPosition = 4410; // any sample; it has to arrive unchanged
} // namespace

TEST_CASE("message: the four messages the engine acts on carry everything they were given") {
    SUBCASE("note on") {
        const std::optional<Event> event = decode_message(MidiMessage{0x91, 60, 100}, kPosition);
        REQUIRE(event.has_value());
        CHECK(event->type == EventType::kNoteOn);
        CHECK(event->tick == kPosition);
        CHECK(event->channel == 1);
        CHECK(event->note == 60);
        CHECK(event->velocity == 100);
    }
    SUBCASE("note off, whose second byte is a release velocity and is not used") {
        const std::optional<Event> event = decode_message(MidiMessage{0x80, 48, 77}, kPosition);
        REQUIRE(event.has_value());
        CHECK(event->type == EventType::kNoteOff);
        CHECK(event->note == 48);
        CHECK(event->velocity == 0); // striking velocity would be a wrong answer, not an unused one
    }
    SUBCASE("control change, including the sustain pedal") {
        const std::optional<Event> event = decode_message(MidiMessage{0xB2, 64, 127}, kPosition);
        REQUIRE(event.has_value());
        CHECK(event->type == EventType::kControlChange);
        CHECK(event->channel == 2);
        CHECK(event->note == Event::kSustainPedal);
        CHECK(event->velocity == 127);
    }
    SUBCASE("pitch bend keeps all fourteen bits, because 8192 is where the wheel rests") {
        const std::optional<Event> rest = decode_message(MidiMessage{0xE3, 0x00, 0x40}, kPosition);
        REQUIRE(rest.has_value());
        CHECK(rest->type == EventType::kPitchBend);
        CHECK(rest->value == Event::kBendCenter);
        CHECK(rest->bend_normalized() == doctest::Approx(0.0F));

        // LSB first: 0x03 low, 0x60 high is 12291, a third of the way up.
        const std::optional<Event> up = decode_message(MidiMessage{0xE0, 0x03, 0x60}, kPosition);
        REQUIRE(up.has_value());
        CHECK(up->value == 12291);
        CHECK(up->bend_normalized() == doctest::Approx((12291.0 - 8192.0) / 8192.0));

        // Bottom of the travel, where dividing by 8192 instead of 8192.0 would round the wrong way.
        const std::optional<Event> down = decode_message(MidiMessage{0xE0, 0x00, 0x00}, kPosition);
        REQUIRE(down.has_value());
        CHECK(down->bend_normalized() == doctest::Approx(-1.0));
    }
}

TEST_CASE("message: note on with velocity zero stays a note on, because the engine ends the note") {
    // Every keyboard and half of all encoders write a note off this way. Passing it through is a
    // decision, not an omission: see Engine::accept, which is the one place that acts on it. A
    // decoder that turned it into a note off here would work until the day the two answers differed
    // - which is how this file's first version leaked a voice per such note by testing the wrong
    // byte entirely.
    const std::optional<Event> event = decode_message(MidiMessage{0x90, 45, 0}, kPosition);
    REQUIRE(event.has_value());
    CHECK(event->type == EventType::kNoteOn);
    CHECK(event->velocity == 0);
    CHECK(event->note == 45);
}

TEST_CASE("message: what the engine cannot use is refused here, once, by everybody") {
    // 0xA0 polyphonic aftertouch, 0xC0 program change, 0xD0 channel pressure. System messages do
    // not reach a decoder that is asked about channel voices, and do not get an event either.
    const MidiMessage refused[] = {
        MidiMessage{0xA0, 60, 64}, MidiMessage{0xC9, 89, 0},  MidiMessage{0xDF, 100, 0},
        MidiMessage{0xF0, 0, 0},   MidiMessage{0xF8, 0, 0},   MidiMessage{0xFE, 0, 0},
    };
    for (const MidiMessage& message : refused) {
        INFO("status byte " << static_cast<int>(message.status));
        CHECK_FALSE(decode_message(message, kPosition).has_value());
    }
}
