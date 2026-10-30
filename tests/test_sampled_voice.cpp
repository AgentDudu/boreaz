#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/engine/sampled_voice.h"
#include "boreaz/engine/voice_allocator.h"

using boreaz::engine::Instrument;
using boreaz::engine::NoteRequest;
using boreaz::engine::Params;
using boreaz::engine::SampledVoice;
using boreaz::engine::VoiceSample;
using boreaz::engine::VoiceAllocator;

namespace {

constexpr double kRate = 44100.0;
constexpr int kBlock = 512;

// An instrument made of one buffer, because the thing under test is the read-head and not the
// decoder. The engine may not know what a file is (see the hygiene test), so the voice is handed
// this instead.
class FakeInstrument : public Instrument {
  public:
    VoiceSample sample;
    int low_key = 21;
    int high_key = 108;

    const VoiceSample* find(int key, int) const override {
        if (absent)
            return nullptr;
        return (key >= low_key && key <= high_key) ? &sample : nullptr;
    }
    bool empty() const override { return absent; }
    std::size_t resident_bytes() const override { return 0; }
    std::size_t refused_regions() const override { return 0; }

    bool absent = false;
};

// A ramp that is not a sine: every frame is a different number, so a read-head that skipped or
// repeated a frame is visible in the output instead of being mistaken for a waveform.
std::vector<float> ramp_frames(int frames, float scale = 1.0f) {
    std::vector<float> data(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
        data[static_cast<std::size_t>(i)] = scale * std::sin(0.37f * static_cast<float>(i)) * 0.5f;
    return data;
}

NoteRequest struck(int key, float velocity = 1.0f, float bend = 0.0f) {
    NoteRequest request;
    request.key = key;
    request.note = key;
    request.velocity = velocity;
    request.bend_semitones = bend;
    request.frequency = 440.0f * std::pow(2.0f, static_cast<float>(key - 69) / 12.0f);
    return request;
}

float loudest(const std::vector<float>& block) {
    float peak = 0.0f;
    for (const float value : block)
        peak = std::max(peak, std::abs(value));
    return peak;
}

} // namespace

TEST_SUITE("sampled voice") {

TEST_CASE("the read-head advances by the file's rate, the key's pitch, and the bend, in one number") {
    FakeInstrument instrument;
    std::vector<float> pcm = ramp_frames(4000);
    instrument.sample.data = pcm.data();
    instrument.sample.frames = static_cast<std::int64_t>(pcm.size());
    instrument.sample.channels = 1;
    instrument.sample.file_sample_rate = 44100.0;
    instrument.sample.key_center = 60;

    Params params;
    params.pan_spread = 0.0f;
    SampledVoice voice(&instrument);
    voice.start(struck(60), params, kRate, 0);
    REQUIRE(voice.active());
    REQUIRE(voice.sample() != nullptr);
    CHECK(voice.increment() == doctest::Approx(1.0).epsilon(1e-6));

    // An octave is a factor of two, and that is the whole of what a sampled piano does with pitch:
    // no oscillator, no re-drawing of the waveform, only the speed of the read-head.
    voice.start(struck(72), params, kRate, 0);
    CHECK(voice.increment() == doctest::Approx(2.0).epsilon(1e-6));
    voice.start(struck(48), params, kRate, 0);
    CHECK(voice.increment() == doctest::Approx(0.5).epsilon(1e-6));

    // A 44.1 kHz library on a 48 kHz device is in tune, and the same file on a 22.05 kHz device is
    // an octave up. The ratio belongs to the device, not to the file.
    voice.start(struck(60), params, 22050.0, 0);
    CHECK(voice.increment() == doctest::Approx(2.0).epsilon(1e-6));
    instrument.sample.file_sample_rate = 22050.0;
    voice.start(struck(60), params, kRate, 0);
    CHECK(voice.increment() == doctest::Approx(0.5).epsilon(1e-6));
    instrument.sample.file_sample_rate = 44100.0;

    // `tune` in cents, and the wheel. A cent is a hundredth of a semitone either way.
    instrument.sample.tune_cents = 100.0f;
    voice.start(struck(60), params, kRate, 0);
    CHECK(voice.increment() == doctest::Approx(std::pow(2.0, 1.0 / 12.0)).epsilon(1e-6));
    instrument.sample.tune_cents = 0.0f;

    voice.start(struck(60, 1.0f, 2.0f), params, kRate, 0);
    CHECK(voice.increment() == doctest::Approx(std::pow(2.0, 2.0 / 12.0)).epsilon(1e-6));

    // A recording whose own key is elsewhere is shifted by the distance, which is how one sample
    // covers the keys around it.
    instrument.sample.key_center = 55;
    voice.start(struck(60), params, kRate, 0);
    CHECK(voice.increment() == doctest::Approx(std::pow(2.0, 5.0 / 12.0)).epsilon(1e-6));
    instrument.sample.key_center = 60;

    // Bend while the note is held moves the head and keeps its place: a glido, not a restarted
    // sample.
    voice.start(struck(60), params, kRate, 0);
    const double before = voice.position();
    voice.set_frequency(voice.frequency() * 2.0f, params);
    CHECK(voice.increment() == doctest::Approx(2.0).epsilon(1e-6));
    CHECK(voice.position() == doctest::Approx(before));
}

TEST_CASE("a loop is the file's own, and it holds for as long as the key is down") {
    FakeInstrument instrument;
    std::vector<float> pcm = ramp_frames(400);
    instrument.sample.data = pcm.data();
    instrument.sample.frames = static_cast<std::int64_t>(pcm.size());
    instrument.sample.loops = true;
    instrument.sample.loop_start = 100;
    instrument.sample.loop_end = 140;

    Params params;
    params.pan_spread = 0.0f;
    SampledVoice voice(&instrument);
    voice.start(struck(60), params, kRate, 0);

    constexpr std::size_t kSpan = 16384;
    std::vector<float> out(kSpan, 0.0f);
    std::vector<float> unused(kSpan, 0.0f);
    for (std::size_t at = 0; at < kSpan; at += static_cast<std::size_t>(kBlock)) {
        voice.render(out.data() + at, unused.data() + at, kBlock);
        REQUIRE(voice.active());
        // The head never leaves the loop, so it never reaches the end of the file either: 400
        // frames of PCM and a key held for a third of a second is exactly the case that plays a
        // recording out and goes silent in the middle of a chord.
        CHECK(voice.position() >= 99.0);
        CHECK(voice.position() <= 141.0);
    }

    // The last quarter of the span is the loop, so a sample and the one a loop period later are the
    // same frame of the file. Tolerance is absolute because the waveform crosses zero.
    for (std::size_t i = kSpan - 4000; i + 40 < kSpan; ++i)
        CHECK(std::abs(out[i] - out[i + 40]) < 1e-6f);
    CHECK(loudest(out) > 0.1f); // and it is actually sounding, not silently looping nothing
}

TEST_CASE("a recording that runs out fades instead of stopping dead") {
    FakeInstrument instrument;
    std::vector<float> pcm(200, 0.5f); // a steady level, so any step is the voice and not the file
    instrument.sample.data = pcm.data();
    instrument.sample.frames = 200;
    instrument.sample.gain = 1.0f;

    Params params;
    params.pan_spread = 0.0f;
    SampledVoice voice(&instrument);
    voice.start(struck(60), params, kRate, 0);

    std::vector<float> out(2048, 0.0f);
    std::vector<float> unused(out.size(), 0.0f);
    for (std::size_t at = 0; at < out.size(); at += static_cast<std::size_t>(kBlock))
        voice.render(out.data() + at, unused.data() + at, kBlock);

    // Where the file ends, the output is still near full level and the frame after it is a little
    // smaller. A hard stop would be one sample of full level followed by a zero, which is a click:
    // the difference between those two shapes is what this checks.
    CHECK(out[199] == doctest::Approx(0.5f * 0.70710678f).epsilon(0.02).scale(1.0));
    CHECK(out[240] < out[199]);
    CHECK(out[240] > 0.0f);
    CHECK(std::abs(out[600]) < 1e-6f);

    // Silence is not enough: the voice has to hand itself back, or a 200-frame recording played on
    // a 16-voice pool fills the pool with voices that have finished.
    CHECK_FALSE(voice.active());
    CHECK(voice.amplitude() == 0.0f);

    // And the biggest step anywhere in the block is smaller than the fade itself, which is the
    // click test stated without a listening test.
    float biggest_step = 0.0f;
    for (std::size_t i = 1; i < out.size(); ++i)
        biggest_step = std::max(biggest_step, std::abs(out[i] - out[i - 1]));
    CHECK(biggest_step < 0.05f);
}

TEST_CASE("velocity moves the level when the mapping says it may, and not when it says not to") {
    FakeInstrument instrument;
    std::vector<float> pcm(4000, 0.5f);
    instrument.sample.data = pcm.data();
    instrument.sample.frames = static_cast<std::int64_t>(pcm.size());

    Params params;
    params.pan_spread = 0.0f;

    auto peak_at = [&](float velocity, float tracking) {
        instrument.sample.velocity_tracking = tracking;
        SampledVoice voice(&instrument);
        voice.start(struck(60, velocity), params, kRate, 0);
        std::vector<float> out(2048, 0.0f);
        std::vector<float> unused(out.size(), 0.0f);
        for (std::size_t at = 0; at < out.size(); at += static_cast<std::size_t>(kBlock))
            voice.render(out.data() + at, unused.data() + at, kBlock);
        return loudest(out);
    };

    // `amp_veltrack=100`: the whole curve. A key struck at a third of the way is a third of the
    // level, which is the difference between a piano and an organ.
    const float soft = peak_at(0.33f, 1.0f);
    const float hard = peak_at(1.0f, 1.0f);
    CHECK(hard > 0.3f);
    // `amp_veltrack=100` keeps the engine's own curve, which is velocity-squared-ish and not a
    // straight line: a key at a third of the way is about a sixth of the level, and that is what
    // makes pp audible beside ff instead of merely quieter.
    const float curve = 0.33f * (0.25f + 0.75f * 0.33f);
    CHECK(soft == doctest::Approx(hard * curve).epsilon(0.05));

    // `amp_veltrack=0`: the file at its own level, which is what a single-layer library wants or
    // every note arrives at fortissimo.
    CHECK(peak_at(0.33f, 0.0f) == doctest::Approx(peak_at(1.0f, 0.0f)).epsilon(0.01));

    // Half-way is half of the *difference* between flat and full, so a full strike is unchanged and
    // a soft one is lifted halfway towards it. That is what the opcode means in percent, and it is
    // why a single-layer library can keep its peaks and still have some dynamics.
    const float half = 0.33f * (0.25f + 0.75f * 0.33f);
    CHECK(peak_at(0.33f, 0.5f) == doctest::Approx(hard * (1.0f + half) * 0.5f).epsilon(0.05));
    CHECK(peak_at(1.0f, 0.5f) == doctest::Approx(hard).epsilon(0.01));
}

TEST_CASE("a mono recording fills both channels and the key decides where it sits") {
    FakeInstrument instrument;
    // Long enough that even the fastest read-head this test asks about - key 108 over a recording
    // made for key 60, which is sixteen times speed - still has file left to read.
    std::vector<float> pcm(20000, 0.4f);
    instrument.sample.data = pcm.data();
    instrument.sample.frames = static_cast<std::int64_t>(pcm.size());

    Params centred;
    centred.pan_spread = 0.0f;
    SampledVoice voice(&instrument);
    voice.start(struck(60), centred, kRate, 0);
    std::vector<float> left(kBlock, 0.0f);
    std::vector<float> right(kBlock, 0.0f);
    voice.render(left.data(), right.data(), kBlock);
    // One channel of PCM into a stereo bus: the file has no image, so it gets the middle one, and
    // `width` in the mapping is ignored because there is nothing to widen.
    for (int i = 200; i < kBlock; ++i)
        CHECK(left[static_cast<std::size_t>(i)] == doctest::Approx(right[static_cast<std::size_t>(i)]));

    // The 88-key spread is the engine's own pan, so a sampled piano lines up with a synthesised one
    // instead of arriving down the middle of the field whatever key was pressed.
    Params spread;
    spread.pan_spread = 1.0f;
    SampledVoice bass(&instrument);
    bass.start(struck(21), spread, kRate, 0);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    bass.render(left.data(), right.data(), kBlock);
    const float bass_left = loudest(left);
    const float bass_right = loudest(right);
    CHECK(bass_left > bass_right);

    SampledVoice treble(&instrument);
    treble.start(struck(108), spread, kRate, 0);
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    treble.render(left.data(), right.data(), kBlock);
    CHECK(loudest(right) > loudest(left));
    // Equal power and not equal amplitude: the two ends are mirror-panned, and what stays the same
    // between them is the total, so a bass note is not a quieter piano.
    const float bass_power = bass_left * bass_left + bass_right * bass_right;
    const float treble_power = loudest(left) * loudest(left) + loudest(right) * loudest(right);
    CHECK(bass_power == doctest::Approx(treble_power).epsilon(0.02));
}

TEST_CASE("a key the instrument does not have is not a voice") {
    FakeInstrument instrument;
    instrument.absent = true; // the library loaded, and this key is not in it

    Params params;
    SampledVoice voice(&instrument);
    voice.start(struck(60), params, kRate, 0);
    // Silent, and not holding a place in the pool: a voice that occupies a slot while doing nothing
    // is how an instrument with gaps steals its own silence away from the keys it does have.
    CHECK_FALSE(voice.active());
    CHECK(voice.sample() == nullptr);

    std::vector<float> left(kBlock, 0.0f);
    std::vector<float> right(kBlock, 0.0f);
    voice.render(left.data(), right.data(), kBlock);
    CHECK(loudest(left) == 0.0f);
    CHECK(loudest(right) == 0.0f);

    VoiceAllocator pool(4, &instrument);
    NoteRequest request = struck(60);
    pool.allocate(request, params, kRate, 0);
    CHECK(pool.active_count() == 0);
    CHECK(pool.steal_count() == 0);
}

TEST_CASE("the pool an instrument is given is the pool it stays, and stealing still works") {
    FakeInstrument instrument;
    std::vector<float> pcm(400, 0.25f);
    instrument.sample.data = pcm.data();
    instrument.sample.frames = 400;
    instrument.sample.loops = true;
    instrument.sample.loop_start = 0;
    instrument.sample.loop_end = 100;

    Params params;
    VoiceAllocator sampled(2, &instrument);
    const NoteRequest low = struck(40);
    const NoteRequest middle = struck(60);
    sampled.allocate(low, params, kRate, 1);
    sampled.allocate(middle, params, kRate, 2);
    CHECK(sampled.active_count() == 2);
    CHECK(sampled.steal_count() == 0);

    // A third note into a two-voice pool takes the older one, and the older one is the one that
    // started at tick 1 - the rule the synth pool uses, applied to voices that came from a file.
    sampled.allocate(struck(80), params, kRate, 3);
    CHECK(sampled.steal_count() == 1);
    CHECK(sampled.active_count() == 2);

    // Resizing keeps the kind: `reallocate(4)` in a test, or a polyphony control, must not quietly
    // hand the pool back to the oscillator.
    sampled.reallocate(4);
    REQUIRE(sampled.pool_size() == 4);
    sampled.allocate(struck(50), params, kRate, 4);
    // Whichever slot the new note landed in, it reads a file: that is all "the pool kept its kind"
    // means, and the pool is free to reuse a stolen slot to do it.
    bool any_sampled = false;
    for (int i = 0; i < sampled.pool_size(); ++i)
        any_sampled = any_sampled || sampled.voice(i).sample() != nullptr;
    CHECK(any_sampled);

    // And without an instrument the same class still does what it always did.
    VoiceAllocator synthesised(2);
    synthesised.allocate(low, params, kRate, 1);
    CHECK(synthesised.active_count() == 1);
    CHECK(synthesised.voice(0).sample() == nullptr);
}

TEST_CASE("letting the key go takes the recording with it, over the release time and no faster") {
    FakeInstrument instrument;
    std::vector<float> pcm(4000, 0.5f);
    instrument.sample.data = pcm.data();
    instrument.sample.frames = static_cast<std::int64_t>(pcm.size());

    Params params;
    params.amp_release_s = 0.1f; // 4 410 samples: long enough to measure, short enough to test
    SampledVoice voice(&instrument);
    voice.start(struck(60), params, kRate, 0);

    std::vector<float> out(8192, 0.0f);
    std::vector<float> unused(out.size(), 0.0f);
    voice.render(out.data(), unused.data(), 512); // past the attack
    voice.release(params, 512);
    CHECK(voice.active());
    CHECK(voice.released());

    std::fill(out.begin(), out.end(), 0.0f);
    std::fill(unused.begin(), unused.end(), 0.0f);
    for (std::size_t at = 0; at < out.size(); at += static_cast<std::size_t>(kBlock))
        voice.render(out.data() + at, unused.data() + at, kBlock);

    // Half the release time in, about half the level is left: a release that ignored the parameter
    // would be either instant or eternal, and both are wrong in a way a player notices at once.
    const float quarter = out[1100];
    const float three_quarters = out[3300];
    CHECK(quarter > 0.15f);
    CHECK(quarter > three_quarters);
    CHECK(three_quarters > 0.0f);
    CHECK(std::abs(out[5000]) < 1e-6f);
    CHECK_FALSE(voice.active()); // and the voice is back in the pool, not parked at zero
}

TEST_CASE("the pedal hands a held note to the allocator without stopping it") {
    FakeInstrument instrument;
    std::vector<float> pcm(4000, 0.5f);
    instrument.sample.data = pcm.data();
    instrument.sample.frames = static_cast<std::int64_t>(pcm.size());

    Params params;
    VoiceAllocator pool(2, &instrument);
    pool.allocate(struck(60), params, kRate, 1);
    REQUIRE(pool.active_count() == 1);

    // Pedal down, key up: the note is not released, it is `silence`d - the state that means "keep
    // sounding, but I am free to take you".
    pool.note_off(0, 60, params, 2, /*pedal_down=*/true);
    CHECK(pool.voice(0).sustained());
    CHECK(pool.voice(0).active());

    std::vector<float> left(kBlock, 0.0f);
    std::vector<float> right(kBlock, 0.0f);
    pool.render(left.data(), right.data(), kBlock);
    CHECK(loudest(left) > 0.05f); // still sounding, whatever the key is doing

    // Pedal up: now it really is released, and the ramp is the release time and not a cut.
    pool.pedal(false, params, 10);
    CHECK(pool.voice(0).released());
    pool.render(left.data(), right.data(), kBlock);
    bool quieter = false;
    for (int i = 0; i < 4; ++i) {
        std::fill(left.begin(), left.end(), 0.0f);
        std::fill(right.begin(), right.end(), 0.0f);
        pool.render(left.data(), right.data(), kBlock);
        quieter = quieter || loudest(left) < 0.5f;
    }
    CHECK(quieter);
}

TEST_CASE("a pan change mid-note arrives on the next block, and nothing else changes") {
    FakeInstrument instrument;
    std::vector<float> pcm(4000, 0.5f);
    instrument.sample.data = pcm.data();
    instrument.sample.frames = static_cast<std::int64_t>(pcm.size());

    Params params;
    params.pan_spread = 0.0f;
    SampledVoice voice(&instrument);
    voice.start(struck(80), params, kRate, 0);

    Params wider = params;
    wider.pan_spread = 1.0f;
    const double before = voice.position();
    voice.refresh(wider, false);
    CHECK(voice.position() >= before); // refresh never restarts the recording

    std::vector<float> left(kBlock, 0.0f);
    std::vector<float> right(kBlock, 0.0f);
    voice.render(left.data(), right.data(), kBlock);
    // A high key with the field open belongs on the right.
    CHECK(loudest(right) > loudest(left));
}

} // TEST_SUITE
