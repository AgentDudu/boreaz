// The display's data path, without a display. What is asserted here is what the window shows and
// what it refuses to show, because those are the claims the phase makes: a held key is
// distinguishable from one the pedal is holding, the compass beyond A0-C8 is admitted rather than
// folded onto a neighbour, and the switcher's rows are the panels there are.
//
// The pixels themselves are not here. Which key a click hits is KeyboardLayout's arithmetic and
// test_keyboard_layout.cpp decides that; what is left - ImGui calling Direct3D - is the part of a
// GUI that cannot be tested without a screen, and is the smallest part of a GUI that can break.
#include <doctest/doctest.h>

#include <string>

#include "boreaz/engine/engine.h"
#include "boreaz/engine/event.h"
#include "boreaz/engine/params.h"
#include "boreaz/gui/computer_keyboard.h"
#include "boreaz/gui/frame_view.h"
#include "boreaz/gui/keyboard_layout.h"
#include "boreaz/gui/panel.h"

#include "../tests/engine_fixture.h"

using boreaz::engine::Event;
using boreaz::engine::Params;
using boreaz::gui::DeviceStats;
using boreaz::gui::FrameView;
using boreaz::gui::KeyboardLayout;
using boreaz::gui::MidiStats;
using boreaz::gui::PanelKind;
using boreaz::gui::panel;
using boreaz::gui::panel_table;
using fixture::kRate;
using fixture::render;

namespace {

FrameView view_of(const boreaz::engine::Engine& engine) {
    return boreaz::gui::make_frame_view(engine, DeviceStats{}, MidiStats{});
}

const boreaz::gui::Sounding* find(const FrameView& view, int note) {
    for (int index = 0; index < view.sounding_count; ++index)
        if (view.sounding[index].note == note)
            return &view.sounding[index];
    return nullptr;
}

} // namespace

TEST_SUITE_BEGIN("gui frame view");

TEST_CASE("a frame view tells a held key from one the pedal is holding") {
    INFO("The keyboard paints both, and the only way a player learns the difference is if the data "
         "carries it: a pedal that looks like a held key is a display that lies.");
    Params params;
    boreaz::engine::Engine engine(params);
    engine.prepare(kRate);

    INFO("hold a key, then release it");
    engine.push_event(Event::note_on(0, 0, 69, 100));
    render(engine, static_cast<int>(kRate));
    FrameView held = view_of(engine);
    CHECK(held.sounding_count == 1);
    REQUIRE(find(held, 69) != nullptr);
    CHECK(find(held, 69)->held);
    CHECK(find(held, 69)->channel == 0);
    // The velocity the player typed, not the envelope's current value: a key fades but the thing
    // that was struck stays struck at the force it was struck with.
    CHECK(find(held, 69)->velocity == doctest::Approx(100).epsilon(0.02));

    // A slice of the release, not a whole second of it: the default amp_release_s is 0.6 s, and a
    // test that renders past the end of the tail is asserting that silence is silent.
    engine.push_event(Event::note_off(0, 0, 69));
    render(engine, 20);
    FrameView released = view_of(engine);
    INFO("the note is still sounding, so it is still on screen; the key is up, so it is not held");
    CHECK(released.active_voices == 1);
    REQUIRE(find(released, 69) != nullptr);
    CHECK_FALSE(find(released, 69)->held);

    INFO("the same note, with the sustain pedal down, is sounding and not held either");
    boreaz::engine::Engine pedalled(params);
    pedalled.prepare(kRate);
    pedalled.push_event(Event::control(0, 0, 64, 127));
    pedalled.push_event(Event::note_on(0, 0, 69, 90));
    render(pedalled, static_cast<int>(kRate));
    pedalled.push_event(Event::note_off(0, 0, 69));
    render(pedalled, static_cast<int>(kRate));
    const FrameView sustained = view_of(pedalled);
    CHECK(sustained.sustain);
    REQUIRE(find(sustained, 69) != nullptr);
    CHECK_FALSE(find(sustained, 69)->held);
}

TEST_CASE("notes outside the compass are counted, not folded onto a neighbouring key") {
    INFO("A 128 note instrument driving an 88 key display has notes with nowhere to go. Lighting "
         "A0 for a C0 would tell the player their ears are wrong, and silence would look like a "
         "dropped event. The count says it instead.");
    Params params;
    params.transpose_semitones = 0;
    boreaz::engine::Engine engine(params);
    engine.prepare(kRate);
    engine.push_event(Event::note_on(0, 0, 12, 90)); // C0, a full octave below the strip
    render(engine, static_cast<int>(kRate));

    const FrameView view = view_of(engine);
    CHECK(view.active_voices == 1);
    CHECK(view.ext_voices == 1);
    for (int index = 0; index < view.sounding_count; ++index)
        CHECK(KeyboardLayout::contains(view.sounding[index].note));
    CHECK(find(view, 12) == nullptr);

    INFO("and a note inside the compass is not counted as outside");
    boreaz::engine::Engine inside(params);
    inside.prepare(kRate);
    inside.push_event(Event::note_on(0, 0, 21, 90)); // A0, the bottom key that exists
    render(inside, static_cast<int>(kRate));
    const FrameView shown = view_of(inside);
    CHECK(shown.ext_voices == 0);
    REQUIRE(find(shown, 21) != nullptr);
}

TEST_CASE("the frame carries the device and port numbers the strip reads") {
    DeviceStats device;
    device.open = true;
    device.name = "Speakers (Some Driver)";
    device.sample_rate = 48000.0;
    device.callback_frames = 512;
    device.late_callbacks = 2;
    device.restarts = 1;
    device.cpu_fraction = 0.034;
    MidiStats midi;
    midi.open = true;
    midi.name = "Some Keyboard";
    midi.losses = 7;

    Params params;
    boreaz::engine::Engine engine(params);
    engine.prepare(kRate);
    engine.push_event(Event::note_on(0, 0, 60, 100));
    render(engine, static_cast<int>(kRate));

    FrameView view = boreaz::gui::make_frame_view(engine, device, midi);
    view.frames = 120;
    view.seconds = 2.0;
    CHECK(view.device.late_callbacks == 2);
    CHECK(view.midi.losses == 7);
    CHECK(view.active_voices == 1);
    CHECK(view.sample_position == static_cast<std::int64_t>(kRate));
    CHECK(view.frames_per_second() == doctest::Approx(60.0));
    // The clip light is the engine's own pre-limiter peak, so a mix at full scale reaches 1.0 here
    // and the limiter's work stays invisible - which is the point of metering before it.
    CHECK(view.drive_peak >= 0.0f);
    CHECK_FALSE(view.sustain);
}

TEST_CASE("one key with two voices keeps both envelopes apart") {
    INFO("A retrigger during a release is normal. The frame lists voices, not keys, because folding "
         "them is the view's one job and a frame that lost the difference could not show it.");
    Params params;
    boreaz::engine::Engine engine(params);
    engine.prepare(kRate);
    engine.push_event(Event::note_on(0, 0, 64, 120));
    render(engine, 200);
    engine.push_event(Event::note_off(0, 0, 64));
    render(engine, 200);
    engine.push_event(Event::note_on(0, 0, 64, 40));
    render(engine, 200);

    const FrameView view = view_of(engine);
    CHECK(view.active_voices == 2);
    REQUIRE(view.sounding_count == 2);
    int held = 0;
    for (int index = 0; index < view.sounding_count; ++index) {
        CHECK(view.sounding[index].note == 64);
        if (view.sounding[index].held)
            ++held;
    }
    // The new strike is held and the old one is on its way out; both are on the same key, and the
    // view that paints it takes the brighter of the two levels.
    CHECK(held == 1);
}

TEST_SUITE_END();

TEST_CASE("a frame with no song says nothing about a song") {
    INFO("The transport's clock is the position of a song, not the uptime of the driver: the audio "
         "thread counts samples from the moment the device opens, so a panel that prints that number "
         "on an empty screen shows a transport that looks like it is about to move. The gate is this "
         "one flag, which is why it is on the frame the panel reads and not inside the drawing.");
    FrameView empty;
    empty.device.sample_rate = 48000.0;
    empty.sample_position = 48000;
    CHECK_FALSE(empty.song_loaded);
    CHECK(empty.position_seconds() == doctest::Approx(1.0)); // the clock exists and is ticking
    CHECK_FALSE(empty.song_loaded);                          // and it is still not a song
    Params params;
    boreaz::engine::Engine engine(params);
    engine.prepare(kRate);
    render(engine, 512);
    CHECK_FALSE(view_of(engine).song_loaded); // a frame built from a live engine invents none
}

TEST_SUITE("computer keyboard") {

TEST_CASE("the letter rows play two octaves, and every one of them has a key") {
    INFO("The mouse and a MIDI cable both need hardware this test does not have, so the desk layout "
         "is the one playing path that can be pinned down without a screen: a key that mapped to a "
         "note off the strip, or to a neighbour of the note its label promises, would only show up "
         "as someone hunting for a note that should be under their finger.");
    // The rows in order, so a slip in the table shows up as the wrong interval rather than as a
    // number someone retyped to match the code.
    const char* const rows = "zsxdcvgbhnjm,l.q2we3r5t6y7ui9p0";
    int previous = -100;
    for (const char* at = rows; *at != 0; ++at) {
        const int note = boreaz::gui::note_for_computer_key(*at);
        INFO("key '" << *at << "'");
        REQUIRE(note >= 0);
        CHECK(KeyboardLayout::contains(note));
        CHECK(note >= previous - 2); // ascending within a row, so a scale cannot stutter
        previous = note;
    }
    CHECK(boreaz::gui::note_for_computer_key('q') == 60); // middle C, the note the layout is on
    CHECK(boreaz::gui::note_for_computer_key('z') == 48); // an octave below it
    CHECK(boreaz::gui::note_for_computer_key('p') == 76); // and the top of the upper row
}

TEST_CASE("keys the program has not promised anything about do nothing") {
    INFO("An unmapped key has to be distinguishable from note 0, which is a real note: -1 is what "
         "tells the app to drop the keystroke instead of putting a C-1 into the engine.");
    // Letters outside the two rows, punctuation the rows do not use, and the control codes a window
    // manager owns (tab, return, escape, F10).
    const char outside[] = {'a', 'f', 'k', ';', '/', '\t', '\n', '\r'};
    for (const char key : outside) {
        INFO("key " << static_cast<int>(key));
        CHECK(boreaz::gui::note_for_computer_key(key) == -1);
    }
    CHECK(boreaz::gui::note_for_computer_key(27) == -1);  // escape
    CHECK(boreaz::gui::note_for_computer_key(291) == -1); // f10, which has no ASCII meaning
}

TEST_CASE("space is the pedal, held rather than toggled") {
    INFO("The engine already treats CC 64 as a pedal with notes to hand over; the only decision "
         "here is whether the keyboard reports a press or a state, and a foot only knows the state.");
    CHECK(boreaz::gui::key_is_sustain_pedal(' '));
    CHECK_FALSE(boreaz::gui::key_is_sustain_pedal('z'));
    CHECK(boreaz::gui::note_for_computer_key(' ') == -1); // so it cannot be a note and a pedal
}

} // TEST_SUITE
