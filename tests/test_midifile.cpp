#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "boreaz/engine/engine.h"
#include "boreaz/engine/event.h"
#include "boreaz/midifile/midifile.h"

#ifndef BOREAZ_FIXTURE_DIR
#error "BOREAZ_FIXTURE_DIR must be defined for the fixture tests"
#endif
#ifndef BOREAZ_RENDER_TOOL
#error "BOREAZ_RENDER_TOOL must be defined for the render tool tests"
#endif

using boreaz::engine::Event;
using boreaz::engine::EventType;
using boreaz::midifile::LoadResult;
using boreaz::midifile::Song;

namespace {

constexpr double kRate = 44100.0;
using Bytes = std::vector<std::uint8_t>;

// The bytes in these tests are laid out by hand. A parser tested against files its own test code
// wrote can only fail in one direction, so the four committed fixtures come from
// tools/midifile_fixtures.py instead - a different implementation, in a different language - and
// the hazard cases below are literal byte lists.

void push_be(Bytes& out, std::uint32_t value, int width) {
    for (int shift = 8 * (width - 1); shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
}

Bytes varlen(std::uint32_t value) {
    Bytes out;
    do {
        out.insert(out.begin(), static_cast<std::uint8_t>(value & 0x7Fu));
        value >>= 7;
    } while (value != 0);
    for (std::size_t i = 0; i + 1 < out.size(); ++i)
        out[i] = static_cast<std::uint8_t>(out[i] | 0x80u);
    return out;
}

Bytes chunk(const char* id, const Bytes& body) {
    Bytes out{static_cast<std::uint8_t>(id[0]), static_cast<std::uint8_t>(id[1]),
              static_cast<std::uint8_t>(id[2]), static_cast<std::uint8_t>(id[3])};
    push_be(out, static_cast<std::uint32_t>(body.size()), 4);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes file_bytes(int format, int tracks, int division, const Bytes& body) {
    Bytes header;
    push_be(header, static_cast<std::uint32_t>(format), 2);
    push_be(header, static_cast<std::uint32_t>(tracks), 2);
    push_be(header, static_cast<std::uint32_t>(division), 2);
    Bytes out = chunk("MThd", header);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// One track holding exactly these event byte strings.
Bytes one_track(const Bytes& events, int format = 0, int division = 480) {
    Bytes body = events;
    const Bytes end = {0x00, 0xFF, 0x2F, 0x00};
    body.insert(body.end(), end.begin(), end.end());
    const Bytes track_chunk = chunk("MTrk", body);
    return file_bytes(format, 1, division, track_chunk);
}

Bytes cat(Bytes first, const Bytes& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

Bytes meta(std::uint32_t delta, std::uint8_t type, const Bytes& payload) {
    Bytes out = varlen(delta);
    out.push_back(0xFF);
    out.push_back(type);
    out = cat(out, varlen(static_cast<std::uint32_t>(payload.size())));
    return cat(out, payload);
}

Bytes note_on(std::uint32_t delta, int channel, int note, int velocity) {
    return cat(varlen(delta), Bytes{static_cast<std::uint8_t>(0x90 | channel),
                                    static_cast<std::uint8_t>(note),
                                    static_cast<std::uint8_t>(velocity)});
}

Bytes note_off(std::uint32_t delta, int channel, int note) {
    return cat(varlen(delta),
               Bytes{static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(note),
                     0x40});
}

Bytes bend(std::uint32_t delta, int channel, int value) {
    return cat(varlen(delta), Bytes{static_cast<std::uint8_t>(0xE0 | channel),
                                    static_cast<std::uint8_t>(value & 0x7F),
                                    static_cast<std::uint8_t>((value >> 7) & 0x7F)});
}

Bytes control(std::uint32_t delta, int channel, int number, int value) {
    return cat(varlen(delta), Bytes{static_cast<std::uint8_t>(0xB0 | channel),
                                    static_cast<std::uint8_t>(number),
                                    static_cast<std::uint8_t>(value)});
}

Bytes sysex(std::uint32_t delta, const Bytes& payload) {
    Bytes out = cat(varlen(delta), Bytes{0xF0});
    out = cat(out, varlen(static_cast<std::uint32_t>(payload.size())));
    return cat(out, payload);
}

LoadResult load(const Bytes& bytes, double rate = kRate) {
    return boreaz::midifile::parse(bytes.data(), bytes.size(), rate);
}

bool mentions(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& line : lines)
        if (line.find(needle) != std::string::npos)
            return true;
    return false;
}

std::string error_of(const Bytes& bytes) {
    const LoadResult result = load(bytes);
    REQUIRE_FALSE(result.ok);
    return result.error;
}

// A render that is silent from the first sample would be perfectly reproducible too, so say it is
// not: something has to be sounding in the middle of an 88 key glissando, and the length has to be
// what the song asked for.
bool cursor_reached(const Song& song, const std::vector<float>& audio) {
    const std::size_t middle = audio.size() / 2;
    float loudest = 0.0F;
    for (std::size_t i = middle; i < middle + 4096 && i < audio.size(); ++i)
        loudest = std::max(loudest, std::abs(audio[i]));
    return loudest > 1.0e-4F && audio.size() == static_cast<std::size_t>(song.end_sample) * 2u;
}

} // namespace

TEST_CASE("midifile: a file that is not one is refused, and says what is wrong with it") {
    SUBCASE("empty") {
        CHECK(error_of({}).find("too short") != std::string::npos);
    }
    SUBCASE("a RIFF file wearing a MIDI extension") {
        Bytes bytes{'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'};
        CHECK(error_of(bytes).find("MThd") != std::string::npos);
    }
    SUBCASE("a header that is not 6 bytes long") {
        Bytes header;
        push_be(header, 8, 2);
        Bytes bytes{'M', 'T', 'h', 'd'};
        push_be(bytes, 8, 4);
        bytes.insert(bytes.end(), header.begin(), header.end());
        CHECK(error_of(bytes).find("expected 6") != std::string::npos);
    }
    SUBCASE("format 2") {
        const std::string message = error_of(file_bytes(2, 1, 480, {}));
        CHECK(message.find("format 2") != std::string::npos);
        CHECK(message.find("format 0 or 1") != std::string::npos);
    }
    SUBCASE("no tracks") {
        CHECK(error_of(file_bytes(0, 0, 480, {})).find("no tracks") != std::string::npos);
    }
    SUBCASE("SMPTE frame division") {
        // 0xFFE8 is -24 in the specification's own notation: 24 frames a second.
        const std::string message = error_of(file_bytes(1, 1, 0xFFE8, {}));
        CHECK(message.find("SMPTE") != std::string::npos);
        CHECK(message.find("24") != std::string::npos);
    }
    SUBCASE("zero ticks per quarter note") {
        CHECK(error_of(file_bytes(1, 1, 0, {})).find("0 ticks") != std::string::npos);
    }
    SUBCASE("a header cut short") {
        const Bytes bytes{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0};
        CHECK(error_of(bytes).find("mid-header") != std::string::npos);
    }
}

TEST_CASE("midifile: messages become engine events at audio sample positions") {
    // 480 ticks per quarter at 120 bpm is 0.5 s a quarter, so 22050 samples a quarter note.
    Bytes events;
    events = cat(events, meta(0, 0x51, {0x07, 0xA1, 0x20})); // 500000 us = 120 bpm
    events = cat(events, note_on(0, 0, 60, 100));
    events = cat(events, note_off(480, 0, 60));
    events = cat(events, control(0, 2, 64, 127));
    events = cat(events, bend(0, 2, 8192 + 2000));
    const LoadResult loaded = load(one_track(events));
    REQUIRE(loaded.ok);
    REQUIRE(loaded.song.events.size() == 4);

    CHECK(loaded.song.events[0].type == EventType::kNoteOn);
    CHECK(loaded.song.events[0].tick == 0);
    CHECK(loaded.song.events[0].note == 60);
    CHECK(loaded.song.events[0].velocity == 100);
    CHECK(loaded.song.events[0].channel == 0);

    CHECK(loaded.song.events[1].type == EventType::kNoteOff);
    CHECK(loaded.song.events[1].tick == 22050); // 0.5 s at 44100 Hz, exactly
    CHECK(loaded.song.events[1].note == 60);

    CHECK(loaded.song.events[2].type == EventType::kControlChange);
    CHECK(loaded.song.events[2].channel == 2);
    CHECK(loaded.song.events[2].note == Event::kSustainPedal);
    CHECK(loaded.song.events[2].velocity == 127);

    CHECK(loaded.song.events[3].type == EventType::kPitchBend);
    CHECK(loaded.song.events[3].value == 8192 + 2000);
    CHECK(loaded.song.events[3].bend_normalized() == doctest::Approx(2000.0 / 8192.0));
}

TEST_CASE("midifile: note on with velocity zero arrives as it was sent, because the engine decides") {
    // Half of all encoders and every keyboard end a note this way, and phase 3's byte parser made
    // a point of passing it through: the one place that decides what it means is the engine. A file
    // layer that rewrote it would be a second answer to that question, and this file's first one
    // got it wrong by testing the note number instead of the velocity.
    const LoadResult loaded = load(one_track(cat(note_on(0, 3, 45, 90), note_on(10, 3, 45, 0))));
    REQUIRE(loaded.ok);
    REQUIRE(loaded.song.events.size() == 2);
    CHECK(loaded.song.events[0].type == EventType::kNoteOn);
    CHECK(loaded.song.events[0].velocity == 90);
    CHECK(loaded.song.events[1].type == EventType::kNoteOn);
    CHECK(loaded.song.events[1].velocity == 0);
    CHECK(loaded.song.events[1].note == 45);
    CHECK(loaded.song.events[1].channel == 3);
}

TEST_CASE("midifile: running status is honoured, and a stray data byte is not") {
    // 0x90 0x3C 0x64 then a second note with the status byte dropped, as encoders write it.
    const Bytes with_running{0x00, 0x90, 0x3C, 0x64, 0x00, 0x40, 0x64, 0x00, 0xFF, 0x2F, 0x00};
    const LoadResult loaded = load(one_track(with_running));
    REQUIRE(loaded.ok);
    REQUIRE(loaded.song.events.size() == 2);
    CHECK(loaded.song.events[0].note == 0x3C);
    CHECK(loaded.song.events[1].type == EventType::kNoteOn);
    CHECK(loaded.song.events[1].note == 0x40);

    // The same byte with no status before it can only mean the file was cut mid-message.
    const std::string message = error_of(one_track({0x3C, 0x64}));
    CHECK(message.find("status byte was expected") != std::string::npos);
    CHECK(message.find("track 0") != std::string::npos);
}

TEST_CASE("midifile: real time and system common bytes have no business in a file") {
    CHECK(error_of(one_track({0x00, 0xF8})).find("real-time") != std::string::npos);
    CHECK(error_of(one_track({0x00, 0xF2, 0x00, 0x00})).find("system common") != std::string::npos);
    // A 0xF7 that starts an event is a sysex, and the byte after its length is data, not a status.
    const LoadResult loaded = load(one_track(cat(sysex(0, {0x7E, 0x7F, 0x09, 0x01}), note_on(0, 0, 60, 90))));
    REQUIRE(loaded.ok);
    REQUIRE(loaded.song.events.size() == 1);
    CHECK(loaded.song.events[0].note == 60);
    CHECK(mentions(loaded.warnings, "system exclusive"));
}

TEST_CASE("midifile: a file that stops short says where") {
    Bytes whole = one_track(cat(note_on(0, 0, 60, 90), note_on(0, 0, 62, 90)));
    const Bytes cut(whole.begin(), whole.end() - 5);
    const std::string message = error_of(cut);
    INFO(message);
    CHECK(message.find("ends") != std::string::npos);

    // An event that runs past its own track's declared length is the same class of lie.
    Bytes lying{'M', 'T', 'r', 'k'};
    push_be(lying, 5, 4); // the body below is 7 bytes
    const Bytes body = cat(note_on(0, 0, 60, 90), Bytes{0x00, 0xFF, 0x2F, 0x00});
    lying.insert(lying.end(), body.begin(), body.end());
    const std::string past = error_of(file_bytes(0, 1, 480, lying));
    CHECK(past.find("past its declared length") != std::string::npos);
}

TEST_CASE("midifile: a track with no end of track event still plays, with a word said") {
    // The bytes add up exactly, so nothing is lost: refusing this file would only be rude.
    const Bytes body = note_on(0, 0, 60, 90);
    Bytes track_chunk{'M', 'T', 'r', 'k'};
    push_be(track_chunk, static_cast<std::uint32_t>(body.size()), 4);
    track_chunk.insert(track_chunk.end(), body.begin(), body.end());
    const LoadResult loaded = load(file_bytes(0, 1, 480, track_chunk));
    REQUIRE(loaded.ok);
    CHECK(loaded.song.events.size() == 1);
    CHECK(mentions(loaded.warnings, "end of track"));
}

TEST_CASE("midifile: an unknown chunk is skipped and named, never played as a track") {
    // One track is declared, and one track is present, with a chunk we do not know in front of it.
    // The header counts tracks, not chunks, so the junk is skipped on the way to the track rather
    // than mistaken for it - which is what a RIFF or a "fact" chunk ahead of the tracks would
    // otherwise do to a real file.
    Bytes body = file_bytes(1, 1, 480, {});
    const Bytes extra = chunk("foo ", {'n', 'o', 't', 'e', 's'});
    body.insert(body.end(), extra.begin(), extra.end());
    const Bytes events = note_on(0, 0, 60, 90);
    const Bytes track_chunk = chunk("MTrk", cat(events, {0x00, 0xFF, 0x2F, 0x00}));
    body.insert(body.end(), track_chunk.begin(), track_chunk.end());

    const LoadResult loaded = load(body);
    REQUIRE(loaded.ok);
    CHECK(loaded.song.events.size() == 1);
    CHECK(mentions(loaded.warnings, "unknown chunk"));
}

TEST_CASE("midifile: a file that promises tracks it does not deliver is refused, not half played") {
    // Silence is the worst possible outcome of a corrupt file: it looks like success. A header that
    // promises two tracks and supplies one is a broken file, and saying so is the whole point of
    // the exit codes.
    Bytes body = file_bytes(1, 2, 480, chunk("MTrk", cat(note_on(0, 0, 60, 90),
                                                             {0x00, 0xFF, 0x2F, 0x00})));
    const LoadResult loaded = load(body);
    CHECK_FALSE(loaded.ok);
    INFO("said: " << loaded.error);
    CHECK(loaded.error.find("ends after 1 of the 2") != std::string::npos);
}

TEST_CASE("midifile: what cannot be played is dropped out loud, not quietly") {
    Bytes events;
    events = cat(events, meta(0, 0x01, {'p', 'a', 'd'}));       // text: dropped silently, as promised
    events = cat(events, meta(0, 0x7E, {0x01, 0x02}));          // unknown meta
    events = cat(events, Bytes{0x00, 0xC0, 0x59});              // program change
    events = cat(events, Bytes{0x00, 0xA0, 0x3C, 0x40});        // polyphonic aftertouch
    events = cat(events, Bytes{0x00, 0xD0, 0x40});              // channel pressure
    const LoadResult loaded = load(one_track(events));
    REQUIRE(loaded.ok);
    CHECK(loaded.song.events.empty());
    CHECK(mentions(loaded.warnings, "meta event 0x7E"));
    CHECK(mentions(loaded.warnings, "program change"));
    CHECK(mentions(loaded.warnings, "polyphonic aftertouch"));
    CHECK(mentions(loaded.warnings, "channel pressure"));
    // Text is the one thing we understand and do not mention: it changes nothing about the sound.
    CHECK_FALSE(mentions(loaded.warnings, "text"));
}

TEST_CASE("midifile: a broken tempo is refused rather than played at a guess") {
    SUBCASE("zero microseconds a quarter is not a tempo") {
        const LoadResult loaded =
            load(one_track(cat(meta(0, 0x51, {0x00, 0x00, 0x00}), note_on(480, 0, 60, 90))));
        REQUIRE(loaded.ok);
        CHECK(loaded.song.tempos.empty());
        CHECK(mentions(loaded.warnings, "ignored a tempo"));
        // No tempo means MIDI's default 120 bpm, so a quarter is still half a second.
        REQUIRE(loaded.song.events.size() == 1);
        CHECK(loaded.song.events[0].tick == 22050);
    }
    SUBCASE("a tempo event whose length is not 3 bytes") {
        const LoadResult loaded = load(one_track(cat(meta(0, 0x51, {0x07, 0xA1}),
                                                      note_on(480, 0, 60, 90))));
        REQUIRE(loaded.ok);
        CHECK(loaded.song.tempos.empty());
        CHECK(mentions(loaded.warnings, "length is not 3"));
        CHECK(loaded.song.events[0].tick == 22050);
    }
    SUBCASE("an end of track event with data is a corrupt file") {
        Bytes body = note_on(0, 0, 60, 90);
        const Bytes bad = meta(0, 0x2F, {0x01});
        body.insert(body.end(), bad.begin(), bad.end());
        const std::string message = error_of(one_track(body));
        CHECK(message.find("end of track event claims") != std::string::npos);
    }
}

TEST_CASE("midifile: the tempo map moves time, and a tempo in another track still counts") {
    // Halfway through, the quarter note doubles in length. The tick grid does not move; the sample
    // positions after the change do, which is the whole difference between a tempo map and a scale.
    Bytes conductor;
    conductor = cat(conductor, meta(0, 0x51, {0x07, 0xA1, 0x20})); // 120 bpm
    conductor = cat(conductor, meta(480, 0x51, {0x0F, 0x42, 0x40})); // 60 bpm
    conductor = cat(conductor, meta(0, 0x58, {4, 2, 24, 8}));       // 4/4, reported and unused
    Bytes notes;
    notes = cat(notes, note_on(0, 0, 60, 90));
    notes = cat(notes, note_on(480, 0, 62, 90));  // at the tempo change
    notes = cat(notes, note_on(480, 0, 64, 90));  // one quarter later

    Bytes body = chunk("MTrk", cat(conductor, {0x00, 0xFF, 0x2F, 0x00}));
    const Bytes second = chunk("MTrk", cat(notes, {0x00, 0xFF, 0x2F, 0x00}));
    body.insert(body.end(), second.begin(), second.end());

    const LoadResult loaded = load(file_bytes(1, 2, 480, body));
    REQUIRE(loaded.ok);
    REQUIRE(loaded.song.tempos.size() == 2);
    REQUIRE(loaded.song.events.size() == 3);

    CHECK(loaded.song.events[0].tick == 0);
    CHECK(loaded.song.events[1].tick == 22050);  // 0.5 s, at 120 bpm
    CHECK(loaded.song.events[2].tick == 66150);  // plus 1.0 s, at 60 bpm
    CHECK(loaded.song.time_signatures.size() == 1);
    INFO("reported as: " << (loaded.song.time_signatures.empty() ? "(nothing)"
                                                                : loaded.song.time_signatures[0]));
    CHECK(loaded.song.time_signatures[0].find("4/4") != std::string::npos);
    // Tick 480, not 0: the meta events share one track, and a delta time is relative to the tempo
    // event that precedes it. That is exactly the trap this format sets for a parser that reads
    // events as if they were absolute.
    CHECK(loaded.song.time_signatures[0].find("tick 480") != std::string::npos);
    CHECK(loaded.song.time_signatures[0].find("4/2") == std::string::npos); // the byte is an exponent
}

TEST_CASE("midifile: tracks merge in sample order and keep their own order inside one sample") {
    const auto two_tracks = [](int first_channel, int second_channel) {
        Bytes body = chunk("MTrk", cat(note_on(0, first_channel, 60, 90),
                                       {0x00, 0xFF, 0x2F, 0x00}));
        const Bytes second = chunk("MTrk", cat(note_on(0, second_channel, 64, 90),
                                               {0x00, 0xFF, 0x2F, 0x00}));
        body.insert(body.end(), second.begin(), second.end());
        return load(file_bytes(1, 2, 480, body));
    };

    const LoadResult first = two_tracks(0, 1);
    REQUIRE(first.ok);
    REQUIRE(first.song.events.size() == 2);
    CHECK(first.song.events[0].channel == 0); // same sample, so track order decides
    CHECK(first.song.events[1].channel == 1);

    // Swap the tracks and the order has to swap with them, or a render is not reproducible.
    const LoadResult swapped = two_tracks(1, 0);
    REQUIRE(swapped.ok);
    REQUIRE(swapped.song.events.size() == 2);
    CHECK(swapped.song.events[0].channel == 1);
    CHECK(swapped.song.events[1].channel == 0);
}

TEST_CASE("midifile: tick and sample positions agree, and the tail leaves room for the last release") {
    Bytes conductor;
    conductor = cat(conductor, meta(0, 0x51, {0x07, 0xA1, 0x20}));
    conductor = cat(conductor, meta(960, 0x51, {0x03, 0xD0, 0x90})); // 240 bpm
    const Bytes notes = note_on(1920, 0, 60, 90);
    Bytes body = chunk("MTrk", cat(conductor, {0x00, 0xFF, 0x2F, 0x00}));
    const Bytes second = chunk("MTrk", cat(notes, {0x00, 0xFF, 0x2F, 0x00}));
    body.insert(body.end(), second.begin(), second.end());

    const LoadResult loaded = load(file_bytes(1, 2, 480, body));
    REQUIRE(loaded.ok);
    const Song& song = loaded.song;

    for (std::int64_t tick = 0; tick <= 2400; tick += 7) {
        const std::int64_t back = song.sample_to_tick(song.tick_to_sample(tick));
        INFO("tick " << tick << " -> " << song.tick_to_sample(tick) << " -> " << back);
        CHECK(std::abs(back - tick) <= 1); // one sample of rounding is the most this can cost
    }

    REQUIRE(song.events.size() == 1);
    CHECK(song.end_sample == song.events[0].tick + 44100); // one second of release
}

TEST_CASE("midifile: the four committed fixtures parse into what their recipe says") {
    const std::filesystem::path dir{BOREAZ_FIXTURE_DIR};

    SUBCASE("lead-simple: one melody, one tempo, nothing dropped") {
        const LoadResult loaded = boreaz::midifile::load_file((dir / "lead-simple.mid").string(), kRate);
        REQUIRE(loaded.ok);
        CHECK(loaded.warnings.empty());
        CHECK(loaded.song.format == 0);
        CHECK(loaded.song.ticks_per_quarter == 480);
        CHECK(loaded.song.tempos.size() == 1);
        REQUIRE(loaded.song.events.size() == 19); // 8 on, 8 off, 1 volume, 2 bend
        // The volume wheel is the first thing at tick 0 because the recipe puts it there, and the
        // order inside one tick is the order in the file, which is what makes a render reproducible.
        CHECK(loaded.song.events[0].type == EventType::kControlChange);
        CHECK(loaded.song.events[1].type == EventType::kNoteOn);
        CHECK(loaded.song.events[1].note == 60);
        CHECK(loaded.song.events.back().note == 108 - 12); // the recipe's last note: C8 minus an octave
    }

    SUBCASE("bass-running-status: dropped status bytes and velocity zero note offs") {
        const LoadResult loaded =
            boreaz::midifile::load_file((dir / "bass-running-status.mid").string(), kRate);
        REQUIRE(loaded.ok);
        CHECK(loaded.warnings.empty()); // running status is a fact about encoders, not a warning
        REQUIRE(loaded.song.events.size() == 32);
        const int pattern[4] = {21, 33, 40, 45};
        for (std::size_t i = 0; i < loaded.song.events.size(); i += 2) {
            INFO("event " << i);
            CHECK(loaded.song.events[i].type == EventType::kNoteOn);
            CHECK(loaded.song.events[i].velocity == 96);
            CHECK(loaded.song.events[i].note == pattern[(i / 2) % 4]);
            // The release arrives as a status-less running status byte meaning note on, velocity 0.
            CHECK(loaded.song.events[i + 1].type == EventType::kNoteOn);
            CHECK(loaded.song.events[i + 1].velocity == 0);
            CHECK(loaded.song.events[i + 1].note == pattern[(i / 2) % 4]);
        }
    }

    SUBCASE("pad-chords: four tracks, a tempo change in another track, and dropped parts named") {
        const LoadResult loaded = boreaz::midifile::load_file((dir / "pad-chords.mid").string(), kRate);
        REQUIRE(loaded.ok);
        CHECK(loaded.song.format == 1);
        REQUIRE(loaded.song.events.size() == 48); // 24 notes want to sound, and 16 of them will steal
        CHECK(loaded.song.tempos.size() == 2);
        REQUIRE(loaded.song.time_signatures.size() == 2);
        CHECK(loaded.song.time_signatures[1].find("3/4") != std::string::npos);
        CHECK(mentions(loaded.warnings, "program change"));
        CHECK(mentions(loaded.warnings, "system exclusive"));

        // Every note of track 1's voicing before any of track 2's, at one sample.
        std::size_t first_channel_one = loaded.song.events.size();
        for (std::size_t i = 0; i < loaded.song.events.size(); ++i)
            if (loaded.song.events[i].channel == 1) {
                first_channel_one = i;
                break; // the first one: without this the loop reports the last, and 35 looks plausible
            }
        CHECK(first_channel_one == 6);
        CHECK(loaded.song.events[0].tick == loaded.song.events[6].tick);
        // All 24 onsets are the same sample: one chord, four tracks.
        for (std::size_t i = 0; i < 24; ++i)
            CHECK(loaded.song.events[i].tick == loaded.song.events[0].tick);
    }

    SUBCASE("glissando-88: every key a piano has, in order, once") {
        const LoadResult loaded =
            boreaz::midifile::load_file((dir / "glissando-88.mid").string(), kRate);
        REQUIRE(loaded.ok);
        CHECK(loaded.song.tempos.size() == 2); // half tempo, halfway up
        REQUIRE(loaded.song.events.size() == 176);

        bool seen[128] = {};
        int previous_velocity = 0;
        std::int64_t previous_sample = -1;
        int notes = 0;
        for (const Event& event : loaded.song.events) {
            if (event.type != EventType::kNoteOn)
                continue;
            ++notes;
            CHECK(event.note >= 21);
            CHECK(event.note <= 108);
            CHECK(!seen[event.note]); // 88 distinct keys, no duplicates
            seen[event.note] = true;
            CHECK(event.velocity >= previous_velocity); // the recipe's crescendo
            CHECK(event.tick > previous_sample);        // ascending in time as well as pitch
            previous_velocity = event.velocity;
            previous_sample = event.tick;
        }
        CHECK(notes == 88);
        CHECK(seen[21]);   // A0
        CHECK(seen[108]);  // C8
        CHECK(previous_velocity == 120);
    }
}

TEST_CASE("midifile: the same file renders identically twice, which is what a golden is allowed to mean") {
    const std::string path = (std::filesystem::path{BOREAZ_FIXTURE_DIR} / "glissando-88.mid").string();
    const LoadResult first = boreaz::midifile::load_file(path, kRate);
    const LoadResult second = boreaz::midifile::load_file(path, kRate);
    REQUIRE(first.ok);
    REQUIRE(second.ok);
    REQUIRE(first.song.events.size() == second.song.events.size());
    for (std::size_t i = 0; i < first.song.events.size(); ++i) {
        INFO("event " << i);
        CHECK(first.song.events[i].tick == second.song.events[i].tick);
        CHECK(first.song.events[i].type == second.song.events[i].type);
        CHECK(first.song.events[i].note == second.song.events[i].note);
        CHECK(first.song.events[i].velocity == second.song.events[i].velocity);
    }

    const auto render = [&first](const Song& song) {
        boreaz::engine::Engine engine;
        engine.prepare(song.sample_rate);
        std::vector<float> out;
        std::vector<float> block(2 * 256, 0.0f);
        std::size_t cursor = 0;
        for (std::int64_t done = 0; done < song.end_sample; done += 256) {
            const int frames = static_cast<int>(
                std::min<std::int64_t>(256, song.end_sample - done));
            cursor = boreaz::midifile::feed_and_render(engine, song, cursor, block.data(), frames)
                         .cursor;
            out.insert(out.end(), block.begin(), block.begin() + frames * 2);
        }
        return out;
    };

    const std::vector<float> a = render(first.song);
    const std::vector<float> b = render(second.song);
    REQUIRE(a.size() == b.size());
    CHECK(a == b); // byte for byte, or a golden file would be a coin toss
    CHECK(cursor_reached(first.song, a));
}

namespace {

std::string quote(const std::string& text) {
    return "\"" + text + "\"";
}

std::string fixture(const std::string& name) {
    return quote((std::filesystem::path{BOREAZ_FIXTURE_DIR} / name).string());
}

std::string trim(std::string text) {
    const char* whitespace = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(whitespace);
    if (begin == std::string::npos)
        return {};
    return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
}

// cmd.exe strips the first and last quote of a command line that begins with one, so the whole
// line is wrapped in a pair of its own. Without that wrapper every path here is quoted and nothing
// runs - and std::system reports that as 1, the same code a real failure returns, which is how a
// test for a renderer can spend an afternoon blaming the renderer.
std::string shell(const std::string& command) { return "\"" + command + "\""; }

int run_tool(const std::string& arguments) {
    return std::system(shell(quote(BOREAZ_RENDER_TOOL) + " " + arguments + " >nul 2>&1").c_str());
}

int run_tool_captured(const std::string& arguments, const std::filesystem::path& capture) {
    return std::system(shell(quote(BOREAZ_RENDER_TOOL) + " " + arguments + " >" +
                             quote(capture.string()) + " 2>&1")
                            .c_str());
}

std::vector<char> read_binary(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string text{(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
    return text;
}

std::uint32_t le32(const std::vector<char>& bytes, std::size_t at) {
    REQUIRE(at + 4 <= bytes.size());
    const auto b = reinterpret_cast<const unsigned char*>(bytes.data());
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

std::uint16_t le16(const std::vector<char>& bytes, std::size_t at) {
    REQUIRE(at + 2 <= bytes.size());
    const auto b = reinterpret_cast<const unsigned char*>(bytes.data());
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}

bool four_cc(const std::vector<char>& bytes, std::size_t at, const char* what) {
    return bytes.size() >= at + 4 &&
           std::equal(what, what + 4, bytes.begin() + static_cast<std::ptrdiff_t>(at));
}

// The loudest sample in the file, as a fraction of full scale: enough to tell a quiet render from
// a loud one without a decoder.
double wav_peak(const std::vector<char>& bytes) {
    const std::uint32_t data = le32(bytes, 40);
    const std::size_t samples = data / 2;
    double loudest = 0.0;
    for (std::size_t i = 0; i < samples; ++i) {
        const std::int16_t sample = static_cast<std::int16_t>(le16(bytes, 44 + i * 2));
        loudest = std::max(loudest, static_cast<double>(std::abs(static_cast<int>(sample))) / 32768.0);
    }
    return loudest;
}

} // namespace

TEST_CASE("boreaz-render: a MIDI file in, a WAV file out, and twice is the same file") {
    const auto dir = std::filesystem::temp_directory_path() / "boreaz_render_tests";
    std::filesystem::create_directories(dir);
    const auto a = dir / "lead.wav";
    const auto twice = dir / "lead_twice.wav";
    const auto quiet = dir / "lead_quiet.wav";
    const auto low = dir / "lead_low_rate.wav";
    const auto params = dir / "quiet.params";
    const auto bogus = dir / "bogus.params";
    const auto capture = dir / "captured.txt";

    {
        std::ofstream out(params);
        out << "# the loudest thing in the mix is the master gain\nmaster_gain_db = -40\n";
    }
    {
        std::ofstream out(bogus);
        out << "master_gian_db = -40\n"; // a typo is a mistake, not something to guess at
    }

    CHECK(run_tool(fixture("lead-simple.mid") + " " + quote(a.string())) == 0);
    const std::vector<char> wav = read_binary(a);
    INFO("output size: " << wav.size());
    REQUIRE(wav.size() > 44);
    CHECK(four_cc(wav, 0, "RIFF"));
    CHECK(four_cc(wav, 8, "WAVE"));
    CHECK(four_cc(wav, 12, "fmt "));
    CHECK(le16(wav, 20) == 1);                // PCM, not a codec
    CHECK(le16(wav, 22) == 2);                // stereo, because the engine is
    CHECK(le32(wav, 24) == 44100);
    CHECK(le16(wav, 34) == 16);
    CHECK(four_cc(wav, 36, "data"));
    CHECK(le32(wav, 40) == wav.size() - 44);  // the sizes were patched, not left as placeholders
    CHECK(le32(wav, 4) == wav.size() - 8);

    // Same command, same bytes. This is the criterion the plan calls CI-comparable: a machine
    // without a sound card has to be able to check what the synth did.
    CHECK(run_tool(fixture("lead-simple.mid") + " " + quote(twice.string())) == 0);
    CHECK(read_binary(twice) == wav);

    // Half the rate, half the frames, same seconds. Plus or minus a frame: the release tail is
    // seconds times rate, and that product rounds differently at the two rates.
    CHECK(run_tool("--rate 22050 " + fixture("lead-simple.mid") + " " + quote(low.string())) == 0);
    const std::vector<char> half = read_binary(low);
    CHECK(le32(half, 24) == 22050);
    CHECK(std::llabs(static_cast<long long>(le32(half, 40)) -
                     static_cast<long long>((wav.size() - 44) / 2)) <= 2);

    // A parameter file that works, and one that does not.
    const double loud = wav_peak(wav);
    CHECK(loud > 0.01);
    CHECK(run_tool(fixture("lead-simple.mid") + " " + quote(quiet.string()) + " " +
                   quote(params.string())) == 0);
    CHECK(wav_peak(read_binary(quiet)) < loud * 0.1);
    CHECK(run_tool(fixture("lead-simple.mid") + " " + quote(quiet.string()) + " " +
                   quote(bogus.string())) == 2);

    // Exit statuses, the ones a script can act on.
    CHECK(run_tool(quote((dir / "nope.mid").string()) + " " + quote(a.string())) == 3);
    CHECK(run_tool(fixture("lead-simple.mid") + " " + quote((dir / "sub" / "x.wav").string())) ==
          4);

    // The golden mode the committed measurements come from.
    CHECK(run_tool_captured("--golden " + fixture("lead-simple.mid"), capture) == 0);
    const std::string line = trim(read_text(capture));
    INFO(line);
    CHECK(line.find("name=lead-simple.mid") != std::string::npos);
    CHECK(line.find("rate=44100") != std::string::npos);
    CHECK(line.find("fnv1a64=") != std::string::npos);
    CHECK(line.find("frames=") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("boreaz-render: the committed goldens still match a render") {
    const std::filesystem::path golden_path{
        std::string{BOREAZ_SOURCE_DIR} + "/tests/golden/midifile.txt"};
    REQUIRE(std::filesystem::exists(golden_path));

    const auto dir = std::filesystem::temp_directory_path() / "boreaz_golden_tests";
    std::filesystem::create_directories(dir);
    const auto capture = dir / "line.txt";

    std::ifstream in(golden_path);
    std::string expected;
    int checked = 0;
    while (std::getline(in, expected)) {
        expected = trim(expected);
        if (expected.empty() || expected[0] == '#')
            continue;
        const std::size_t equals = expected.find("name=");
        REQUIRE(equals == 0);
        const std::size_t space = expected.find(' ', equals);
        const std::string name = expected.substr(equals + 5, space - 5);

        REQUIRE(run_tool_captured("--golden " + fixture(name), capture) == 0);
        // The tool says out loud what it dropped, on stderr, and the capture merges the two streams.
        // The golden is the measurement, so take that line and nothing else: a fixture gaining a
        // warning is worth knowing about, but it is not audio drift.
        const std::string captured = read_text(capture);
        const std::size_t start = captured.find("name=");
        INFO("the tool printed no measurement line at all: " << captured);
        REQUIRE(start != std::string::npos);
        const std::string actual = trim(captured.substr(start, captured.find('\n', start) - start));
        INFO("golden drift for " << name << "\n  committed: " << expected << "\n  rendered:  "
                                << actual);
        // If this fails on purpose - a new oscillator, a different limiter - regenerate the file
        // with the command in --help and say why in the commit message. A golden that is quietly
        // refreshed has stopped protecting anything.
        CHECK(actual == expected);
        ++checked;
    }
    CHECK(checked == 4);
    std::filesystem::remove_all(dir);
}
