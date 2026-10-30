#include <algorithm>
#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/engine/synth_voice.h"
#include "boreaz/engine/voice_allocator.h"

using boreaz::engine::NoteRequest;
using boreaz::engine::Params;
using boreaz::engine::SynthVoice;
using boreaz::engine::VoiceAllocator;

namespace {

constexpr double kRate = 44100.0;

NoteRequest request(int note, float velocity = 0.8f, int channel = 0) {
    NoteRequest value;
    value.note = note;
    value.key = note;
    value.channel = static_cast<std::uint8_t>(channel);
    value.velocity = velocity;
    // The engine normally fills this from the Tuning LUT; a voice only needs a pitch.
    value.frequency = 440.0f * std::pow(2.0f, static_cast<float>(note - 69) / 12.0f);
    return value;
}

// Renders and returns the peak, so a test can say "this made sound" without a buffer of its own.
float render(SynthVoice& voice, int frames) {
    std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), 0.0f);
    voice.render(left.data(), right.data(), frames);
    float peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        peak = std::max({peak, std::fabs(left[static_cast<std::size_t>(i)]),
                         std::fabs(right[static_cast<std::size_t>(i)])});
    }
    return peak;
}

std::vector<float> render_stereo(SynthVoice& voice, int frames) {
    std::vector<float> left(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> right(static_cast<std::size_t>(frames), 0.0f);
    voice.render(left.data(), right.data(), frames);
    left.insert(left.end(), right.begin(), right.end());
    return left;
}

} // namespace

TEST_CASE("a started voice makes sound and stays inside full scale") {
    SynthVoice voice;
    Params params;
    voice.start(request(60), params, kRate, 0);
    CHECK(voice.active());
    CHECK_FALSE(voice.released());

    const float peak = render(voice, 4410); // 100 ms
    CHECK(peak > 1.0e-3f);
    CHECK(peak <= 1.0f);
}

TEST_CASE("release hands the voice back to the pool when the envelope ends") {
    SynthVoice voice;
    Params params;
    params.amp_release_s = 0.05f;
    voice.start(request(60), params, kRate, 0);

    voice.release(params, 1000);
    CHECK(voice.released());
    render(voice, 2000); // 45 ms of a 50 ms release
    CHECK(voice.active());

    render(voice, 44100); // a second later it must be gone
    CHECK_FALSE(voice.active());
    CHECK(voice.amplitude() == doctest::Approx(0.0f).epsilon(0.01));
    // A finished voice adds nothing, which is the difference between 16 voices and 16 ghosts.
    CHECK(render(voice, 256) == 0.0f);
}

TEST_CASE("note off during the attack still releases instead of sticking") {
    SynthVoice voice;
    Params params;
    params.amp_attack_s = 0.2f;
    params.amp_release_s = 0.02f;
    voice.start(request(60), params, kRate, 0);
    render(voice, 441); // halfway up the attack
    voice.release(params, 441);
    render(voice, 44100);
    CHECK_FALSE(voice.active());
}

TEST_CASE("the sustain pedal keeps a note after its key goes up, then lets it go") {
    SynthVoice voice;
    Params params;
    params.amp_release_s = 0.05f;
    params.amp_sustain = 0.5f;
    voice.start(request(60), params, kRate, 0);
    render(voice, 4410);

    voice.silence(4410);
    CHECK(voice.sustained());
    CHECK(voice.active());
    CHECK(render(voice, 4410) > 1.0e-3f); // still sounding on the pedal

    voice.release(params, 8820);
    CHECK_FALSE(voice.sustained());
    CHECK(voice.released());
    render(voice, 44100);
    CHECK_FALSE(voice.active());
}

TEST_CASE("the same note renders the same bytes twice, noise transient included") {
    Params params;
    SynthVoice a;
    a.start(request(48, 1.0f), params, kRate, 12345);
    SynthVoice b;
    b.start(request(48, 1.0f), params, kRate, 12345);
    // Includes the hammer noise, whose generator must be seeded from the note and its tick and
    // never from a clock, or byte-identical renders are impossible.
    CHECK(render_stereo(a, 8820) == render_stereo(b, 8820));
}

TEST_CASE("a parameter change retunes a decaying note without striking it again") {
    SynthVoice voice;
    Params params;
    params.amp_attack_s = 0.001f;
    params.amp_decay_s = 0.05f;
    params.amp_sustain = 0.1f;
    voice.start(request(60), params, kRate, 0);
    render(voice, 8820); // 200 ms in: decayed to sustain
    const float before = voice.amplitude();
    CHECK(before < 0.2f);

    params.filter_cutoff_ratio = 3.0f;
    params.filter_resonance = 0.9f;
    params.pan_spread = 0.0f;
    voice.refresh(params, false);

    render(voice, 441);
    // A re-trigger would show up here as the amplitude jumping back towards 1.0.
    CHECK(voice.amplitude() < before * 1.5f);
    CHECK(voice.amplitude() > 0.0f);
}

TEST_CASE("wild parameters cannot make a voice non-finite") {
    SynthVoice voice;
    Params params;
    params.sanitize();
    params.filter_cutoff_ratio = 1.0e30f;
    params.amp_attack_s = 1.0e30f;
    params.osc_detune_cents = -1.0e30f;
    params.noise_decay_s = 0.0f;
    params.sanitize();
    voice.start(request(21, 1.0f), params, kRate, 0);

    std::vector<float> left(2048, 0.0f);
    std::vector<float> right(2048, 0.0f);
    voice.render(left.data(), right.data(), 2048);
    for (std::size_t i = 0; i < left.size(); ++i) {
        CHECK(std::isfinite(left[i]));
        CHECK(std::isfinite(right[i]));
        CHECK(std::fabs(left[i]) <= 1.0f);
    }
}

TEST_CASE("a voice re-pitched by bend keeps its phases and its sanity") {
    SynthVoice voice;
    Params params;
    voice.start(request(60), params, kRate, 0);
    const float original = voice.frequency();
    voice.set_frequency(original * 1.122462f, params); // +2 semitones
    CHECK(voice.frequency() == doctest::Approx(original * 1.122462f));
    CHECK(render(voice, 1024) > 1.0f * 1.0e-3f);
    // A nonsense pitch is refused rather than latched.
    voice.set_frequency(std::nanf(""), params);
    CHECK(voice.frequency() == doctest::Approx(original * 1.122462f));
}

TEST_CASE("pan spread puts the bass on the left and the treble on the right") {
    Params params;
    SynthVoice bass;
    bass.start(request(21), params, kRate, 0);
    SynthVoice treble;
    treble.start(request(108), params, kRate, 0);

    std::vector<float> bl(2205, 0.0f);
    std::vector<float> br(2205, 0.0f);
    bass.render(bl.data(), br.data(), 2205);
    const float bass_left = *std::max_element(bl.begin(), bl.end());

    std::vector<float> tl(2205, 0.0f);
    std::vector<float> tr(2205, 0.0f);
    treble.render(tl.data(), tr.data(), 2205);
    const float treble_right = *std::max_element(tr.begin(), tr.end());

    CHECK(bass_left > *std::max_element(br.begin(), br.end()));
    CHECK(treble_right > *std::max_element(tl.begin(), tl.end()));
}

TEST_CASE("zero frames render nothing and change nothing") {
    SynthVoice voice;
    Params params;
    voice.start(request(60), params, kRate, 0);
    std::vector<float> left(4, 0.25f);
    std::vector<float> right(4, -0.25f);
    voice.render(left.data(), right.data(), 0);
    CHECK(left[0] == 0.25f);
    CHECK(right[3] == -0.25f);
}
