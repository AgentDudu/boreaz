// The transport's clock: when it starts, what it reads, and when it stops.
//
// The audio thread counts samples from the moment the device opens. A panel that shows that number
// shows how long the driver has been up, and a window left open while someone makes coffee is not a
// performance. These are the rules that keep the two apart.

#include <doctest/doctest.h>

#include "boreaz/gui/transport_clock.h"

using boreaz::gui::TransportClock;

TEST_SUITE("transport clock") {

TEST_CASE("an open window with nothing played has no transport") {
    INFO("Ten minutes of an idle audio stream is sixty seconds of position and zero seconds of "
         "music. This is the exact case a player sees first, so it is the case the clock is written "
         "against: silence does not have a position, and until something asks for music the number "
         "stays at zero and the panel stays at rest.");
    TransportClock clock;
    clock.update(0.0, false);
    clock.update(60.0, false);
    clock.update(600.0, false);
    CHECK_FALSE(clock.running());
    CHECK(clock.seconds() == 0.0);
}

TEST_CASE("the first note is at zero, not at the device's uptime") {
    INFO("The transport takes its zero from the thing that started it, so a note played three "
         "minutes after opening the window reads 0.000 and not 180.000.");
    TransportClock clock;
    clock.update(180.0, false);
    CHECK_FALSE(clock.running());

    clock.update(180.5, true); // the first note, half a second into this block
    CHECK(clock.running());
    CHECK(clock.seconds() == doctest::Approx(0.0));

    clock.update(183.5, true);
    CHECK(clock.seconds() == doctest::Approx(3.0));
}

TEST_CASE("a gap between notes is not a stopped transport") {
    INFO("`input_present` opens the transport and never closes it. A rolled chord, a rest, and a "
         "note whose release outlives its key are all gaps, and a clock that stopped in one would "
         "reset the position of a song that is plainly still going.");
    TransportClock clock;
    clock.update(10.0, true);
    clock.update(12.0, false); // nothing sounding now
    clock.update(25.0, false);
    CHECK(clock.running());
    CHECK(clock.seconds() == doctest::Approx(15.0));
}

TEST_CASE("a parked transport keeps the time it reached") {
    INFO("At the end of a song the transport parks rather than rewinding or scrolling on: the "
         "number left on screen is the song's length, which is a fact, and the playhead sits at the "
         "end of the strip, which is where the music stopped. It stays parked while the room is "
         "quiet - a device running is not music - and starts again on music, because a player who "
         "sits down after the file wants their own playing measured and not the last song's length.");
    TransportClock clock;
    clock.update(0.0, true);
    clock.update(74.0, true);
    clock.halt();
    clock.update(90.0, false); // the device is still running; nothing is playing
    CHECK(clock.running());    // still showing a position, just not a moving one
    CHECK(clock.seconds() == doctest::Approx(74.0));

    SUBCASE("and a new note starts a new clock") {
        clock.update(91.0, true);
        CHECK(clock.seconds() == doctest::Approx(0.0)); // the new song's zero is now, not 74 ago
        clock.update(95.5, true);
        CHECK(clock.seconds() == doctest::Approx(4.5));
    }

    SUBCASE("and the player can rub the number out") {
        clock.reset(90.0);
        CHECK(clock.seconds() == doctest::Approx(0.0));
    }
}

TEST_CASE("a reopened device does not run the transport backwards") {
    INFO("An audio device can be reopened - an exclusive-mode grab, a sample rate change - and the "
         "new stream starts counting from zero again. A transport that subtracted the old start from "
         "the new position would read negative, and a panel would spend the rest of the session "
         "drawing a playhead off the left edge of its own window.");
    TransportClock clock;
    clock.update(300.0, true);
    clock.update(301.0, true);
    CHECK(clock.seconds() == doctest::Approx(1.0));

    clock.update(0.02, true); // a new stream, a new zero
    CHECK(clock.seconds() == doctest::Approx(0.0));
    clock.update(2.02, true);
    CHECK(clock.seconds() == doctest::Approx(2.0));
}

TEST_CASE("resetting the clock counts from the frame that asked") {
    INFO("A transport you cannot set back to zero is a stopwatch with extra steps: time a scale "
         "run, read the number, reset, run it again. The new zero comes from the audio position of "
         "the frame that asked, so the frame after it reads one frame later and not the driver's "
         "uptime.");
    TransportClock clock;
    clock.update(100.0, true);
    clock.update(130.0, true);
    CHECK(clock.seconds() == doctest::Approx(30.0));

    clock.reset(130.0);
    CHECK(clock.seconds() == doctest::Approx(0.0));
    clock.update(131.5, true);
    CHECK(clock.seconds() == doctest::Approx(1.5));
}

TEST_CASE("a reset un-parks the clock and does not switch it on") {
    INFO("Two edges. A song that ended left the clock parked, and asking for zero then means "
         "\"from here\", so it moves again. A clock that never started - nothing played yet - stays "
         "hidden: resetting it must not print 0:00.000 in a window that has made no sound, which is "
         "the promise this class exists to keep.");
    TransportClock parked;
    parked.update(10.0, true);
    parked.update(20.0, true);
    parked.halt();
    parked.reset(20.0);
    parked.update(23.0, true);
    CHECK(parked.seconds() == doctest::Approx(3.0));

    TransportClock idle;
    idle.reset(500.0);
    CHECK_FALSE(idle.running());
    CHECK(idle.seconds() == doctest::Approx(0.0));
    idle.update(501.0, false);
    CHECK_FALSE(idle.running());
    CHECK(idle.seconds() == doctest::Approx(0.0));
}

} // TEST_SUITE
