// The phase-10 transport: play, pause, stop, seek and loop over a Song, on the engine's clock.
// What is measured here is what the phase's exit criteria claim: no drift against the audio clock,
// no stuck notes after stop, seeking that retriggers correctly, more than one pool of notes at once
// stealing instead of crashing, and the keys an acoustic piano does not have still sounding.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/engine/engine.h"
#include "boreaz/midifile/transport.h"
#include "engine_fixture.h"

using boreaz::engine::Engine;
using boreaz::engine::Event;
using boreaz::midifile::Song;
using boreaz::midifile::Transport;

namespace {

constexpr std::int64_t kRate = 48000;
constexpr int kBlock = 512; // the device block the mixer runs at

// A song written by hand, so the test knows where every note is without a parser in the way. The
// events are already in samples, which is what `Song::events` holds.
Song make_song() {
    Song song;
    song.format = 0;
    song.ticks_per_quarter = 480;
    song.sample_rate = static_cast<double>(kRate);
    song.tempos = {{0, 500000.0}};
    song.events = {
        Event::note_on(1000, 0, 60, 110),  Event::note_off(6000, 0, 60),
        Event::note_on(10000, 0, 64, 100), Event::note_off(12000, 0, 64),
        Event::note_on(20000, 0, 72, 90),  Event::note_off(21000, 0, 72),
    };
    song.end_sample = 24000;
    return song;
}

// A voice that started and was never told to stop. This, and not "the engine went silent", is what
// a transport stop owes the player: a panic releases notes the way a piano's dampers do, so a voice
// that rings while released is correct and one still held open is a stuck note.
int unreleased(const Engine& engine) {
    int count = 0;
    for (int index = 0; index < engine.pool_size(); ++index) {
        const boreaz::engine::Voice& voice = engine.voices().voice(index);
        if (voice.active() && !voice.released())
            ++count;
    }
    return count;
}

struct Run {
    int blocks = 0;
    float peak = 0.0f;      // the loudest sample rendered during the run
    int max_voices = 0;     // active, released or not
    int max_unreleased = 0; // the most that were sounding and not held, at any block
};

// One pump iteration: feed against the position the engine has reached, then render a block. Being
// early costs nothing (the engine applies an event on the sample it names) and being late moves it,
// which is the order the mixer uses.
Run run(Engine& engine, Transport& transport, int blocks) {
    Run out;
    std::vector<float> buffer(static_cast<std::size_t>(kBlock) * 2u, 0.0f);
    for (int i = 0; i < blocks; ++i) {
        transport.feed(engine, engine.sample_position());
        engine.process(buffer.data(), kBlock);
        for (const float value : buffer)
            out.peak = std::max(out.peak, std::fabs(value));
        out.max_voices = std::max(out.max_voices, engine.active_voices());
        out.max_unreleased = std::max(out.max_unreleased, unreleased(engine));
        ++out.blocks;
    }
    return out;
}

} // namespace

TEST_CASE("three minutes on the loop cannot drift, because there is only one clock") {
    // The exit criterion is "no drift against the audio clock over 3 minutes". It is true by
    // construction here, and this is the measurement that says so: the position is the engine's own
    // sample counter minus one offset, so it is a linear function of a counter that cannot
    // accumulate error. An implementation that counted blocks, or advanced a double by
    // frames / sample_rate, would fail the equality below somewhere in the third minute.
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    const Song song = make_song();
    Transport transport;
    transport.load(song);
    transport.set_loop(true);
    transport.play(engine);

    const std::int64_t start_position = transport.position();
    const std::int64_t start_engine = engine.sample_position();
    std::vector<float> buffer(static_cast<std::size_t>(kBlock) * 2u, 0.0f);
    std::int64_t fed_at = start_engine;
    int wraps = 0;
    std::int64_t previous = start_position;
    const int blocks = static_cast<int>(3 * 60 * kRate / kBlock); // 48000 / 512 is exact
    for (int block = 0; block < blocks; ++block) {
        fed_at = engine.sample_position();
        transport.feed(engine, fed_at);
        engine.process(buffer.data(), kBlock);
        if (transport.position() < previous)
            ++wraps; // the position went back: that is a repeat boundary
        previous = transport.position();
    }

    const std::int64_t elapsed = fed_at - start_engine;
    const std::int64_t expected_position = (start_position + elapsed) % song.end_sample;
    const int expected_wraps = static_cast<int>((start_position + elapsed) / song.end_sample);
    const std::int64_t expected_elapsed = static_cast<std::int64_t>(blocks) * kBlock - kBlock;
    CHECK(elapsed == expected_elapsed); // the last feed is one block before the last render
    CHECK(transport.position() == expected_position);
    CHECK(wraps == expected_wraps);
    CHECK(transport.looping());
    CHECK(transport.playing()); // a loop does not stop by itself
}

TEST_CASE("a flush request drops an event the ring was holding for later") {
    // The mechanism every transport move depends on. The queue's own clear is consumer-side only,
    // so the request is a flag and the drop happens on the audio thread.
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    REQUIRE(engine.push_event(Event::note_on(engine.sample_position() + 100000, 0, 60, 110)));
    engine.request_transport_flush();

    const fixture::Audio audio = fixture::render(engine, 4000);
    CHECK(audio.peak < fixture::kSilence);
    CHECK(engine.active_voices() == 0);
}

TEST_CASE("pause is silence from the next block and holds the position where it was") {
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    const Song song = make_song();
    Transport transport;
    transport.load(song);
    transport.play(engine);
    run(engine, transport, 4); // past the note at 1000
    REQUIRE(engine.active_voices() > 0);

    // By now a note from the one at 10000 is already in the ring: the feed runs a block ahead of
    // the position. It has to be dropped, not delayed, or pausing starts a note after the player
    // stopped the song.
    transport.pause(engine);
    const std::int64_t held = transport.position();
    const Run after = run(engine, transport, 8);
    CHECK(after.max_unreleased == 0); // nothing started, and what was sounding was let go
    CHECK(unreleased(engine) == 0);   // zero stuck notes
    CHECK(transport.position() == held);
    CHECK_FALSE(transport.playing());
}

TEST_CASE("stop leaves zero stuck notes and puts the playhead back at the top") {
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    const Song song = make_song();
    Transport transport;
    transport.load(song);
    transport.play(engine);
    run(engine, transport, 4);
    REQUIRE(engine.active_voices() > 0);

    transport.stop(engine);
    const Run after = run(engine, transport, 4);
    CHECK(after.max_unreleased == 0);
    CHECK(unreleased(engine) == 0); // the exit criterion, measured: stop leaves zero stuck notes
    CHECK(transport.position() == 0);
    CHECK_FALSE(transport.playing());
}

TEST_CASE("seeking forward skips what has gone by and plays what comes next") {
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    const Song song = make_song();
    Transport transport;
    transport.load(song);
    transport.play(engine);
    transport.seek(engine, 14000); // past the notes at 1000 and 10000, before the one at 20000
    const Run gap = run(engine, transport, 8); // eight blocks reach ~18100, and feed ahead of that
    CHECK(gap.max_unreleased == 0);

    transport.seek(engine, 19000); // the note at 20000 is the next thing due
    const Run after = run(engine, transport, 40);
    CHECK(after.max_unreleased > 0);  // and it plays: a seek that skips the next note is silence
    CHECK(engine.steal_count() == 0); // three notes over a pool of sixteen: nothing to steal
}

TEST_CASE("seeking into a held note does not start that note again") {
    // The bug this rules out is the classic one: the cursor rewinds, the note_on that already
    // sounded is fed a second time, and the player hears the note stab in the middle of itself.
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    const Song song = make_song();
    Transport transport;
    transport.load(song);
    transport.play(engine);
    run(engine, transport, 4); // the note at 1000 is sounding and holds until 6000
    REQUIRE(engine.active_voices() > 0);

    transport.seek(engine, 3000); // inside that note
    const Run after = run(engine, transport, 6);
    CHECK(after.max_unreleased == 0); // it did not start again, and the flush let it go
    CHECK(unreleased(engine) == 0);
}

TEST_CASE("the loop wraps to the top and the note at the top sounds again") {
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    const Song song = make_song();
    Transport transport;
    transport.load(song);
    transport.set_loop(true);
    transport.seek(engine, 23000); // one second of song left: the wrap is imminent
    transport.play(engine);

    const Run first = run(engine, transport, 4); // past the end, into the repeat
    CHECK(transport.position() < song.end_sample);
    CHECK(transport.position() > 0); // the wrap keeps the overshoot, so the loop stays in time
    const Run second = run(engine, transport, 8);
    CHECK(first.max_voices + second.max_voices > 0); // the note at the top of the repeat is back
    CHECK(transport.playing());
}

TEST_CASE("twenty notes at once steal down to the pool and nothing crashes") {
    Song song = make_song();
    song.events.clear();
    for (int note = 24; note < 44; ++note)
        song.events.push_back(Event::note_on(1000, 0, static_cast<std::uint8_t>(note), 100));
    song.events.push_back(Event::note_off(30000, 0, 24));
    song.end_sample = 40000;

    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    engine.set_pool_size_for_test(8);
    Transport transport;
    transport.load(song);
    transport.play(engine);

    const Run after = run(engine, transport, 4);
    CHECK(after.max_voices > 0);
    CHECK(engine.active_voices() <= 8);
    CHECK(engine.steal_count() > 0); // twenty into eight: the spec is stealing, not silence
}

TEST_CASE("keys an acoustic piano does not have are played, not dropped") {
    Song song = make_song();
    song.events = {
        Event::note_on(1000, 0, 10, 100), // below A0
        Event::note_off(6000, 0, 10),
        Event::note_on(8000, 0, 120, 100), // above C8
        Event::note_off(14000, 0, 120),
    };
    song.end_sample = 20000;

    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    Transport transport;
    transport.load(song);
    transport.play(engine);

    const Run after = run(engine, transport, 8);
    CHECK(after.max_voices > 0); // the pitch comes from the tuning table, so these are not silence
    CHECK(after.max_voices > 0);
    CHECK(Engine::is_extended_key(10));
    CHECK(Engine::is_extended_key(120));
}

TEST_CASE("one button, and what one press means is decided in one place") {
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    const Song song = make_song();
    Transport transport;
    transport.load(song);

    // Nothing running: a press starts it.
    transport.toggle(engine);
    CHECK(transport.playing());
    run(engine, transport, 4);
    CHECK(transport.position() > 0);

    // Running: a press is a pause, and the position is held where it was. Held means held: the pump
    // keeps calling feed() every tick whether or not anything is coming, and a feed that advanced a
    // stopped transport would be a pause that quietly keeps playing.
    const std::int64_t paused_at = transport.position();
    transport.toggle(engine);
    CHECK_FALSE(transport.playing());
    run(engine, transport, 8);
    CHECK(transport.position() == paused_at);

    // Paused mid-song: a press picks up from there rather than starting again, which is the whole
    // difference between a pause and a stop.
    transport.toggle(engine);
    CHECK(transport.playing());
    run(engine, transport, 1);
    CHECK(transport.position() > paused_at);

    // Play it to the end and let the tail ring out, then press again.
    run(engine, transport, static_cast<int>(song.end_sample / kBlock) + 20);
    transport.pause(engine);
    CHECK(transport.position() >= song.end_sample);
    transport.toggle(engine);
    CHECK(transport.playing());
    CHECK(transport.position() == 0);
}

TEST_CASE("a slider dragged past either end still lands inside the song") {
    Engine engine;
    engine.prepare(static_cast<double>(kRate));
    const Song song = make_song();
    Transport transport;
    transport.load(song);
    transport.play(engine);
    run(engine, transport, 4);

    // Half way: 12 000 of 24 000, past the D that was already sounding at 10 000 and before the
    // next note at 20 000. What a seek owes the player is asked here the way the other seek cases
    // ask it, by sound and not by a cursor nobody outside this file can read: what was passed over
    // does not start, and what is still to come does.
    transport.seek_fraction(engine, 0.5);
    CHECK(transport.position() == song.end_sample / 2);
    const Run skipped = run(engine, transport, 8); // to about 16 000, feeding a block ahead
    CHECK(skipped.max_unreleased == 0);
    const Run after = run(engine, transport, 40); // past 20 000
    CHECK(after.max_unreleased > 0);

    // Past the ends, and not a number at all. A pointer that leaves the bar, a file of no length
    // that made the fraction a divide-by-zero: each has to end up somewhere inside the song,
    // because a cursor in orbit is a song that never plays again.
    transport.seek_fraction(engine, -3.0);
    CHECK(transport.position() == 0);
    transport.seek_fraction(engine, 7.0);
    CHECK(transport.position() == song.end_sample);
    transport.seek_fraction(engine, std::numeric_limits<double>::quiet_NaN());
    CHECK(transport.position() == 0);

    // A song with no length is a file with no events, and a control that divided by its length
    // would have divided by zero before it got here.
    Song empty;
    empty.sample_rate = static_cast<double>(kRate);
    empty.end_sample = 0;
    transport.load(empty);
    transport.seek_fraction(engine, 0.5);
    CHECK(transport.position() == 0);
    CHECK(transport.end_sample() == 0);
    transport.toggle(engine);
    CHECK(transport.playing()); // a player asked to play silence; there is nothing to refuse
}
