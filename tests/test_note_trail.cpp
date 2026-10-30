// The memory a transport scrolls: a note that has stopped must stay a note.
//
// The engine's telemetry is a photograph of now, so everything these tests are about is what the
// display adds to that photograph - that a note began somewhere, that it ended, and that it is
// remembered until it has had time to leave the strip. No engine and no window are needed: the
// snapshot is POD, and filling it by hand is the difference between testing the bookkeeping and
// testing the audio thread.

#include "boreaz/gui/note_trail.h"

#include <doctest/doctest.h>

#include <cstdint>

namespace {

using boreaz::gui::FrameView;
using boreaz::gui::NoteTrail;
using boreaz::gui::TrailNote;

constexpr double kRate = 48000.0;

// A frame whose transport is at `seconds` with nothing sounding.
FrameView frame_at(double seconds) {
    FrameView view;
    view.device.sample_rate = kRate;
    view.transport_running = true;
    view.transport_seconds = seconds;
    return view;
}

// One sounding voice in the frame. `age_samples` is what the engine says the voice has been running,
// which is the only reason a note has an onset the display did not have to guess.
void sound(FrameView& view, std::uint8_t key, std::uint8_t channel, std::int64_t age_samples) {
    REQUIRE(view.sounding_count < FrameView::kMaxVoices);
    boreaz::gui::Sounding& voice = view.sounding[static_cast<std::size_t>(view.sounding_count++)];
    voice.note = key;
    voice.key = key;
    voice.channel = channel;
    voice.velocity = 100;
    voice.level = 0.5f;
    voice.age_samples = age_samples;
}

const TrailNote* only(const FrameView& view) {
    REQUIRE(view.trail_count == 1);
    return &view.trail[0];
}

} // namespace

TEST_CASE("a note keeps its bar after the voice that made it is gone") {
    NoteTrail trail;

    // Started 0.1 s before this frame, from the voice's own age and not from when the trail first
    // noticed it: a note that began before the window was looking has to be placed where it began.
    FrameView view = frame_at(2.0);
    sound(view, 60, 0, 4800);
    trail.update(view);
    const TrailNote* note = only(view);
    CHECK(note->sounding());
    CHECK(note->onset_seconds == doctest::Approx(1.9));
    CHECK(note->end_seconds < 0.0);

    // Still sounding four hundred milliseconds on. One entry, still open, and the onset unchanged:
    // a frame that sees the same note is not a frame that sees a new one.
    view = frame_at(2.4);
    sound(view, 60, 0, 24000);
    trail.update(view);
    note = only(view);
    CHECK(note->sounding());
    CHECK(note->onset_seconds == doctest::Approx(1.9));

    // Let go. The engine has no voice for it any more, and the strip has a note of the right length
    // to scroll - closed at the second it stopped on, which is what makes a bar fade out instead of
    // blink out.
    view = frame_at(3.5);
    trail.update(view);
    note = only(view);
    CHECK_FALSE(note->sounding());
    CHECK(note->end_seconds == doctest::Approx(3.5));
    CHECK(note->onset_seconds == doctest::Approx(1.9));

    // Remembered until it has had time to leave the strip, and no longer than that.
    view = frame_at(3.5 + NoteTrail::kMemorySeconds * 0.5);
    trail.update(view);
    CHECK(view.trail_count == 1);
    view = frame_at(3.5 + NoteTrail::kMemorySeconds + 0.1);
    trail.update(view);
    CHECK(view.trail_count == 0);
}

TEST_CASE("a key struck twice while the first is ringing is two notes") {
    // With the pedal down, the same key and channel can be sounding twice at once, the first already
    // released. Key and channel cannot tell them apart; the age each voice reports can, and the
    // order they arrive in the snapshot must not decide what gets drawn.
    NoteTrail trail;

    FrameView view = frame_at(1.0);
    sound(view, 60, 0, 0);
    trail.update(view);
    CHECK(view.trail_count == 1);

    view = frame_at(2.0);
    sound(view, 60, 0, 480);   // the second strike, 10 ms ago
    sound(view, 60, 0, 48000); // the first, still sounding
    trail.update(view);
    REQUIRE(view.trail_count == 2);
    CHECK(view.trail[0].onset_seconds == doctest::Approx(1.0));
    CHECK(view.trail[1].onset_seconds == doctest::Approx(1.99));
    CHECK(view.trail[0].sounding());
    CHECK(view.trail[1].sounding());
}

TEST_CASE("two channels on one key are two notes") {
    NoteTrail trail;

    FrameView view = frame_at(1.0);
    sound(view, 60, 0, 0);
    sound(view, 60, 1, 0);
    trail.update(view);
    CHECK(view.trail_count == 2);

    // Both still sounding a second later: neither was mistaken for the other, and neither closed.
    view = frame_at(2.0);
    sound(view, 60, 0, 48000);
    sound(view, 60, 1, 48000);
    trail.update(view);
    REQUIRE(view.trail_count == 2);
    CHECK(view.trail[0].sounding());
    CHECK(view.trail[1].sounding());
    CHECK(view.trail[0].channel == 0);
    CHECK(view.trail[1].channel == 1);
}

TEST_CASE("a clock running backwards starts the memory again") {
    // A song starting over. An entry that began ahead of the playhead would draw a bar reaching to
    // its right, which is a transport running backwards.
    NoteTrail trail;

    FrameView view = frame_at(10.0);
    sound(view, 60, 0, 0);
    trail.update(view);
    CHECK(view.trail_count == 1);

    view = frame_at(0.0);
    trail.update(view);
    CHECK(view.trail_count == 0);
}

TEST_CASE("a frame with no device still places its notes") {
    // Every frame a test builds has no device, so no sample rate and no age to divide by. The
    // honest onset then is now, and nothing about the memory changes.
    NoteTrail trail;

    FrameView view = frame_at(0.0);
    view.device.sample_rate = 0.0;
    sound(view, 60, 0, 9600);
    trail.update(view);
    REQUIRE(view.trail_count == 1);
    CHECK(view.trail[0].onset_seconds == doctest::Approx(0.0));
    CHECK(view.trail[0].sounding());
}
