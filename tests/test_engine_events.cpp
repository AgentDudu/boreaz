#include <algorithm>
#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/engine/engine.h"
#include "engine_fixture.h"

using boreaz::engine::Engine;
using boreaz::engine::Event;
using boreaz::engine::Params;
using fixture::Audio;

TEST_CASE("a note lands on the sample it was scheduled for, not on the block boundary") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));
    REQUIRE(engine.push_event(Event::note_on(100, 0, 60, 110)));

    // One 512-frame block, so the note is scheduled 100 samples into the call.
    const Audio audio = fixture::render(engine, 512, 512);
    for (int frame = 0; frame < 100; ++frame) {
        CAPTURE(frame);
        CHECK(audio.at(static_cast<std::size_t>(frame), 0) == 0.0f);
        CHECK(audio.at(static_cast<std::size_t>(frame), 1) == 0.0f);
    }
    const std::size_t start = audio.first_sound();
    CHECK(start >= 100u);
    CHECK(start < 200u); // the attack is 4 ms, so sound follows within a few samples
    CHECK(engine.sample_position() == 512);
}

TEST_CASE("note on with velocity zero is a note off, because real devices send it") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));
    REQUIRE(engine.push_event(Event::note_on(0, 0, 60, 110)));
    fixture::render(engine, 4410);
    REQUIRE(engine.active_voices() == 1);
    CHECK_FALSE(engine.voices().voice(0).released());

    REQUIRE(engine.push_event(Event::note_on(4410, 0, 60, 0)));
    fixture::render(engine, 4410);
    CHECK(engine.voices().voice(0).released());
}

TEST_CASE("transpose and octave shift move the note and stay on the keyboard") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));
    Params params;
    params.transpose_semitones = 3;
    params.octave_shift = 1;
    engine.set_params(params);
    fixture::render(engine, 1); // a published block takes effect when the next block is processed

    CHECK(engine.sounding_note(60) == 75); // +3 and +12

    REQUIRE(engine.push_event(Event::note_on(0, 0, 60, 110)));
    fixture::render(engine, 441);
    CHECK(engine.voices().voice(0).note() == 75);
    CHECK(engine.voices().voice(0).key() == 60); // the display still knows which key was pressed
    CHECK(engine.voices().voice(0).frequency() == doctest::Approx(engine.tuning().frequency(75)));

    // A key up on the same key finds the transposed voice, because both halves of the pair use
    // the same mapping.
    REQUIRE(engine.push_event(Event::note_off(441, 0, 60)));
    fixture::render(engine, 441);
    CHECK(engine.voices().voice(0).released());

    // Past the end of the LUT the note is clamped, not wrapped or off-by-anything.
    params.transpose_semitones = 24;
    params.octave_shift = 4;
    engine.set_params(params);
    fixture::render(engine, 1);
    CHECK(engine.sounding_note(108) == 127);
    params.transpose_semitones = -24;
    params.octave_shift = -4;
    engine.set_params(params);
    fixture::render(engine, 1);
    CHECK(engine.sounding_note(21) == 0);
}

TEST_CASE("the pitch wheel moves notes already sounding, not only the next one") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));
    REQUIRE(engine.push_event(Event::note_on(0, 0, 69, 110)));
    fixture::render(engine, 4410);
    const float unbent = engine.voices().voice(0).frequency();
    REQUIRE(unbent == doctest::Approx(440.0f));

    // Full travel up: +2 semitones by default, less the half-cent MIDI itself cannot express.
    const float full = (16383.0f - 8192.0f) / 8192.0f;
    REQUIRE(engine.push_event(Event::bend(4410, 0, 16383)));
    fixture::render(engine, 4410);
    CHECK(engine.bend_semitones(0) == doctest::Approx(2.0f * full).epsilon(1.0e-5f));
    CHECK(engine.voices().voice(0).frequency() ==
          doctest::Approx(440.0f * std::pow(2.0f, 2.0f * full / 12.0f)).epsilon(1.0e-5f));

    // Rest, which must be exactly rest and not 8191/8192 of a cent.
    REQUIRE(engine.push_event(Event::bend(8820, 0, 8192)));
    fixture::render(engine, 4410);
    CHECK(engine.bend_semitones(0) == doctest::Approx(0.0f));
    CHECK(engine.voices().voice(0).frequency() == doctest::Approx(440.0f).epsilon(1.0e-5));
}

TEST_CASE("the sustain pedal holds notes, and 64 is the threshold") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));
    REQUIRE(engine.push_event(Event::control(0, 0, Engine::kCcSustainPedal, 127)));
    REQUIRE(engine.push_event(Event::note_on(0, 0, 60, 110)));
    fixture::render(engine, 4410);
    REQUIRE(engine.push_event(Event::note_off(4410, 0, 60)));
    fixture::render(engine, 4410);

    CHECK(engine.sustain_pedal_down());
    CHECK(engine.voices().voice(0).sustained());
    CHECK(engine.active_voices() == 1);

    // A pedal released: the event only becomes state when the audio thread drains it, so
    // render before asking.
    REQUIRE(engine.push_event(Event::control(8820, 0, Engine::kCcSustainPedal, 63)));
    fixture::render(engine, 4410);
    CHECK_FALSE(engine.sustain_pedal_down());
    CHECK(engine.voices().voice(0).released());

    // A pedal that was never pressed must not hold anything.
    Engine bare;
    bare.prepare(static_cast<double>(fixture::kRate));
    REQUIRE(bare.push_event(Event::note_on(0, 0, 60, 110)));
    fixture::render(bare, 4410);
    REQUIRE(bare.push_event(Event::note_off(4410, 0, 60)));
    fixture::render(bare, 4410);
    CHECK(bare.voices().voice(0).released());
    CHECK_FALSE(bare.sustain_pedal_down());
}

TEST_CASE("velocity makes a difference you can hear") {
    auto level = [](int velocity) {
        Engine engine;
        engine.prepare(static_cast<double>(fixture::kRate));
        engine.push_event(Event::note_on(0, 0, 60, static_cast<std::uint8_t>(velocity)));
        return fixture::render(engine, 4410).peak;
    };
    const float soft = level(20);
    const float loud = level(127);
    CHECK(soft > 1.0e-3f);
    CHECK(loud > soft * 2.0f);
    CHECK(loud <= 1.0f);
}

TEST_CASE("a block bigger than the scratch buffer is processed in chunks, clock exact") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));
    engine.push_event(Event::note_on(0, 0, 60, 110));

    std::vector<float> out(static_cast<std::size_t>(1500) * 2u, 0.0f);
    engine.process(out.data(), 1500); // three chunks of the 512-frame scratch
    CHECK(engine.sample_position() == 1500);

    float peak = 0.0f;
    bool finite = true;
    for (const float value : out) {
        finite = finite && std::isfinite(value);
        peak = std::max(peak, std::fabs(value));
    }
    CHECK(finite);
    CHECK(peak > 1.0e-3f);
    CHECK(peak <= 1.0f);

    // Events queued for later in the oversized block still land where they were scheduled.
    Engine timed;
    timed.prepare(static_cast<double>(fixture::kRate));
    timed.push_event(Event::note_on(1000, 0, 60, 110));
    std::vector<float> big(static_cast<std::size_t>(1024) * 2u, 0.0f);
    timed.process(big.data(), 1024);
    std::size_t first = 0;
    while (first < big.size() && big[first] == 0.0f)
        ++first;
    CHECK(first == 2000u); // frame 1000, channel 0: sample-accurate, not block-accurate
    float later = 0.0f;
    for (std::size_t i = 2000; i < big.size(); ++i)
        later = std::max(later, std::fabs(big[i]));
    CHECK(later > 1.0e-3f);
}

TEST_CASE("a full queue drops events instead of blocking the caller") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));

    int accepted = 0;
    for (int i = 0; i < 5000; ++i) {
        if (engine.push_event(
                Event::note_on(0, 0, static_cast<std::uint8_t>(21 + (i % 88)), 100))) {
            ++accepted;
        }
    }
    CHECK(accepted > 0);
    CHECK(accepted < 5000);
    CHECK(engine.dropped_events() > 0);

    // Dropping is a lost note, not a broken engine: it still renders legal audio.
    const Audio audio = fixture::render(engine, 4410);
    CHECK(audio.finite);
    CHECK(audio.peak <= 1.0f);
    CHECK(engine.active_voices() <= Engine::kMaxPolyphony);
}

TEST_CASE("a call with nothing to do does nothing") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));
    engine.process(nullptr, 64);

    std::vector<float> out(128, 0.25f);
    engine.process(out.data(), 0);
    engine.process(out.data(), -8);
    CHECK(out[0] == 0.25f);
    CHECK(engine.sample_position() == 0);

    // An unprepared engine still has to behave, since the offline renderer may just call
    // process() with whatever sample rate the file says.
    Engine unprepared;
    std::vector<float> out2(128, 0.0f);
    unprepared.process(out2.data(), 64);
    CHECK(unprepared.sample_position() == 64);
    for (const float value : out2)
        CHECK(std::isfinite(value));
}

TEST_CASE("controllers nothing has claimed yet are accepted and ignored") {
    Engine engine;
    engine.prepare(static_cast<double>(fixture::kRate));
    engine.push_event(Event::note_on(0, 0, 60, 110));
    fixture::render(engine, 441);

    // Expression, modulation, hold 1, portamento: no meaning yet, so they must change nothing
    // except not crash.
    for (int cc : {2, 11, 122, 65}) {
        REQUIRE(engine.push_event(Event::control(441, 0, static_cast<std::uint8_t>(cc), 100)));
    }
    fixture::render(engine, 441);
    CHECK(engine.active_voices() == 1);
    CHECK_FALSE(engine.voices().voice(0).released());
    CHECK_FALSE(engine.sustain_pedal_down());

    // CC 123 on one channel leaves another channel's note alone.
    Engine split;
    split.prepare(static_cast<double>(fixture::kRate));
    split.push_event(Event::note_on(0, 0, 60, 110));
    split.push_event(Event::note_on(0, 1, 64, 110));
    fixture::render(split, 441);
    REQUIRE(split.active_voices() == 2);
    REQUIRE(split.push_event(Event::control(441, 0, Engine::kCcAllNotesOff, 127)));
    fixture::render(split, 441);
    CHECK(split.voices().voice(0).released());
    CHECK_FALSE(split.voices().voice(1).released());
}
