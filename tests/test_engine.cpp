#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/engine/engine.h"
#include "engine_fixture.h"

using boreaz::engine::Engine;
using boreaz::engine::Event;
using boreaz::engine::Params;
using fixture::Audio;

namespace {

constexpr std::int64_t kSecond = 44100;

std::uint8_t byte(int value) {
    return static_cast<std::uint8_t>(value);
}

// The 16-note chord from the exit criteria: every key of two octaves, pedal down.
void push_chord(Engine& engine, std::int64_t tick, int first = 48, int count = 16,
                int velocity = 100) {
    for (int i = 0; i < count; ++i) {
        REQUIRE(engine.push_event(Event::note_on(tick, 0, byte(first + i), byte(velocity))));
    }
}

} // namespace

TEST_CASE("an engine built with parameters keeps playing them, not falling back to defaults") {
    // Parameters arrive twice: once in the constructor, and once per block from the bus the UI
    // publishes to. An engine that never published its own starting parameters played block one as
    // configured and every block after at defaults, which is how boreaz-render produced byte
    // identical audio for master_gain_db = -40 and -12 and reported success both times.
    // One note at a moderate velocity, on purpose: a four note chord overloads the soft clipper,
    // whose output is not proportional to its input, and a test that measures a saturated sum cannot
    // say anything about a gain 28 dB apart.
    auto peak_with = [](float master_gain_db) {
        Params params;
        params.master_gain_db = master_gain_db;
        Engine engine(params);
        engine.prepare(fixture::kRate);
        REQUIRE(engine.push_event(Event::note_on(0, 0, byte(60), byte(64))));
        return fixture::render(engine, 4410).peak;
    };
    const float loud = peak_with(-12.0F);
    const float quiet = peak_with(-40.0F);
    INFO("loud peak " << loud << ", quiet peak " << quiet << ", ratio " << loud / quiet);
    REQUIRE(loud > fixture::kSilence);
    // Twenty-eight decibels should be a factor of twelve; the test asks for a factor of six, which
    // is still far more than "slightly quieter" and would not budge if a later change moved the
    // master gain by a decibel or two somewhere else.
    CHECK(quiet < loud / 6.0F);

    Params tuned;
    tuned.transpose_semitones = 12;
    Engine engine(tuned);
    engine.prepare(fixture::kRate);
    CHECK(engine.sounding_note(60) == 72); // before a block: what it was built to play
    fixture::render(engine, 64);
    // ...and still the same after a block has been processed, which is the half that broke: the
    // block reads the parameter bus, and a bus nobody published to is a tray of defaults.
    CHECK(engine.sounding_note(60) == 72);
}

TEST_CASE("a sixteen-note chord with the pedal down uses exactly sixteen voices") {
    Engine engine;
    engine.prepare(fixture::kRate);

    REQUIRE(engine.push_event(Event::control(0, 0, Engine::kCcSustainPedal, 127)));
    push_chord(engine, 0);

    const Audio audio = fixture::render(engine, 4410); // 100 ms
    CHECK(engine.active_voices() == 16);
    CHECK(engine.pool_size() == Engine::kMaxPolyphony);
    CHECK(engine.sustain_pedal_down());
    CHECK(audio.finite);
    CHECK(audio.peak > 1.0e-3f);
    CHECK(audio.peak <= 1.0f);
    CHECK(engine.steal_count() == 0); // sixteen notes is the pool, not a steal
}

TEST_CASE("sixteen is a ceiling: a seventeenth note steals, it does not allocate") {
    Engine engine;
    engine.prepare(fixture::kRate);

    for (int i = 0; i < 24; ++i) {
        REQUIRE(engine.push_event(Event::note_on(0, 0, byte(36 + i), 100)));
    }
    const Audio audio = fixture::render(engine, 4410);

    CHECK(engine.active_voices() == 16);
    CHECK(engine.steal_count() == 8);
    CHECK(audio.finite);
    CHECK(audio.peak <= 1.0f);
}

TEST_CASE("panic takes everything off, including what the pedal was holding") {
    Engine engine;
    engine.prepare(fixture::kRate);

    REQUIRE(engine.push_event(Event::control(0, 0, Engine::kCcSustainPedal, 127)));
    push_chord(engine, 0);
    fixture::render(engine, 2205); // 50 ms in, all sixteen sounding
    REQUIRE(engine.active_voices() == 16);

    // Keys up while the pedal is down, then the panic button.
    for (int i = 0; i < 16; ++i) {
        REQUIRE(engine.push_event(Event::note_off(2205, 0, byte(48 + i))));
    }
    REQUIRE(engine.push_event(Event::all_notes_off(2205, 0)));
    const Audio tail = fixture::render(engine, 88200); // 2 s: every release has run out
    CHECK(tail.finite);
    CHECK(engine.active_voices() == 0); // no stuck voices

    // And nothing is left making a noise.
    const Audio silent = fixture::render(engine, 4410);
    CHECK(silent.peak == 0.0f);
}

TEST_CASE("the same events and parameters render the same bytes") {
    auto score = [](Engine& engine) {
        engine.prepare(fixture::kRate);
        Params params;
        params.transpose_semitones = -3;
        params.pan_spread = 0.4f;
        params.noise_level = 0.3f;
        engine.set_params(params);

        engine.push_event(Event::control(0, 0, Engine::kCcSustainPedal, 127));
        push_chord(engine, 0, 36, 12);
        engine.push_event(Event::bend(kSecond / 4, 0, 12000));
        engine.push_event(Event::note_off(kSecond / 2, 0, 40));
        engine.push_event(Event::note_on(kSecond / 2, 0, 72, 25));
        engine.push_event(Event::control(kSecond, 0, Engine::kCcSustainPedal, 0));
        return fixture::render(engine, static_cast<int>(kSecond * 2));
    };

    Engine first;
    Engine second;
    const Audio a = score(first);
    const Audio b = score(second);

    CHECK(a.data == b.data); // byte-identical, which is what makes a golden WAV possible
    CHECK(a.peak > 1.0e-3f);
    CHECK(a.peak == b.peak);
}

TEST_CASE("every key from A0 to C8 makes a sound, stays finite, and stays inside full scale") {
    for (int key = 21; key <= 108; ++key) {
        Engine engine;
        engine.prepare(fixture::kRate);
        REQUIRE(engine.push_event(Event::note_on(0, 0, byte(key), 100)));

        const Audio audio = fixture::render(engine, 4410); // 100 ms is enough for any decay
        INFO("key " << key);
        CHECK(audio.finite);
        CHECK(audio.peak > 1.0e-3f); // no silence, including the 27.5 Hz bottom
        CHECK(audio.peak <= 1.0f);
        CHECK(audio.first_sound() < static_cast<std::size_t>(1024)); // and it starts when asked
    }
}

TEST_CASE("keys outside the piano still play, and are reported as extended range") {
    // The 88-key instrument is A0-C8; MIDI offers 128. The extras must not be silence, and the
    // GUI has to be able to tell the user they are off the keyboard.
    CHECK_FALSE(Engine::is_extended_key(21));
    CHECK_FALSE(Engine::is_extended_key(108));
    for (int key = 0; key <= 127; ++key) {
        const bool expected = key < 21 || key > 108;
        CHECK(Engine::is_extended_key(key) == expected);
    }

    for (int key : {0, 11, 20, 109, 120, 127}) {
        Engine engine;
        engine.prepare(fixture::kRate);
        REQUIRE(engine.push_event(Event::note_on(0, 0, byte(key), 110)));
        const Audio audio = fixture::render(engine, 4410);
        INFO("extended key " << key);
        CHECK(Engine::is_extended_key(key));
        CHECK(audio.finite);
        CHECK(audio.peak > 1.0e-3f); // flagged, never silent
        CHECK(audio.peak <= 1.0f);
    }
}

TEST_CASE("the ceiling holds when everything is loud at once") {
    Engine engine;
    engine.prepare(fixture::kRate);
    Params params;
    params.master_gain_db = 12.0f; // the loudest thing the parameter block allows
    engine.set_params(params);

    for (int key = 21; key <= 108; key += 2) {
        REQUIRE(engine.push_event(Event::note_on(0, 0, byte(key), 127)));
    }
    const Audio audio = fixture::render(engine, 8820);
    CHECK(audio.finite);
    CHECK(audio.peak <= 1.0f);
    CHECK(audio.peak > 0.5f); // and it is actually loud, not quietly clamped
}

TEST_CASE("CC7 is a channel fader: linear, per channel, and heard by a note already sounding") {
    // One note, so the measurement is a fader and not a mix. The peak has to move by the ratio of
    // the two controller values, which is only a true claim while nothing is clipping.
    auto peak_at = [](int fader) {
        Engine engine;
        engine.prepare(fixture::kRate);
        REQUIRE(engine.push_event(Event::note_on(0, 0, byte(60), byte(96))));
        REQUIRE(engine.push_event(
            Event::control(0, 0, Engine::kCcChannelVolume, byte(fader))));
        return fixture::render(engine, 2205).peak;
    };
    const float full = peak_at(127);
    const float half = peak_at(64);
    const float none = peak_at(0);
    INFO("fader 127 -> " << full << ", 64 -> " << half << ", 0 -> " << none);
    REQUIRE(full > fixture::kSilence);
    // Half the controller is half the amplitude: a linear fader, not a dB slider. 2% covers the
    // soft clipper being a hair nonlinear at these levels.
    CHECK(half == doctest::Approx(full * 64.0F / 127.0F).epsilon(0.02));
    CHECK(none <= fixture::kSilence);

    // Per channel, because a general MIDI file quiets its own drum channel and a shared fader would
    // take the piano down with it.
    auto peak_with_one_fader_down = [](int quiet_channel) {
        Engine engine;
        engine.prepare(fixture::kRate);
        REQUIRE(engine.push_event(Event::note_on(0, 0, byte(60), byte(96))));
        REQUIRE(engine.push_event(Event::note_on(0, 1, byte(64), byte(96))));
        REQUIRE(engine.push_event(
            Event::control(0, byte(quiet_channel), Engine::kCcChannelVolume, 0)));
        return fixture::render(engine, 2205).peak;
    };
    const float left_down = peak_with_one_fader_down(0);
    const float right_down = peak_with_one_fader_down(1);
    INFO("channel 0 down -> " << left_down << ", channel 1 down -> " << right_down);
    // Either way one channel is still sounding, so neither fader is a global mute...
    CHECK(left_down > fixture::kSilence);
    CHECK(right_down > fixture::kSilence);
    // ...and both faders down is silence, which is the same fact from the other side: every channel
    // has a fader and none of them is exempt.
    Engine muted;
    muted.prepare(fixture::kRate);
    REQUIRE(muted.push_event(Event::note_on(0, 0, byte(60), byte(96))));
    REQUIRE(muted.push_event(Event::note_on(0, 1, byte(64), byte(96))));
    REQUIRE(muted.push_event(Event::control(0, 0, Engine::kCcChannelVolume, 0)));
    REQUIRE(muted.push_event(Event::control(0, 1, Engine::kCcChannelVolume, 0)));
    CHECK(fixture::render(muted, 2205).peak <= fixture::kSilence);

    // And the reason it is a controller rather than a parameter: a move is heard by the note that is
    // already sounding. A parameter would only reach the next note.
    Engine engine;
    engine.prepare(fixture::kRate);
    REQUIRE(engine.push_event(Event::note_on(0, 0, byte(60), byte(96))));
    const Audio before = fixture::render(engine, 1000);
    REQUIRE(engine.push_event(
        Event::control(engine.sample_position(), 0, Engine::kCcChannelVolume, 0)));
    const Audio after = fixture::render(engine, 1000);
    INFO("before the move -> " << before.peak << ", after -> " << after.peak);
    CHECK(before.peak > fixture::kSilence);
    CHECK(after.peak <= fixture::kSilence);
    CHECK(engine.active_voices() == 1); // the note is still held; it is only inaudible
}
