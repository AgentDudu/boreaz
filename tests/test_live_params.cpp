// Phase 7: parameters that move while the instrument is playing.
//
// Three claims get tested here. The descriptor table is the only place a range or a default lives.
// A parameter that jumps does not click. And a transpose moves the notes that are already sounding
// instead of only the next one.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/config/params_file.h"
#include "boreaz/engine/engine.h"
#include "boreaz/engine/param_table.h"
#include "boreaz/engine/synth_voice.h"
#include "engine_fixture.h"

using boreaz::config::load_params_file;
using boreaz::engine::Engine;
using boreaz::engine::Event;
using boreaz::engine::ParamSpec;
using boreaz::engine::Params;
using boreaz::engine::SynthVoice;
using fixture::Audio;

namespace {

std::uint8_t byte(int value) {
    return static_cast<std::uint8_t>(value);
}

const ParamSpec* find(std::string_view key) {
    return boreaz::engine::param_spec(key);
}

// Biggest sample-to-sample move. A waveform moves by at most its own slope; a discontinuity - a
// click - moves by about the difference between two sounds, which at these levels is hundreds of
// times the slope. The step test below is what proves this measures that.
float largest_jump(const std::vector<float>& samples) {
    float jump = 0.0f;
    for (std::size_t i = 1; i < samples.size(); ++i)
        jump = std::max(jump, std::fabs(samples[i] - samples[i - 1]));
    return jump;
}

float largest_jump(const Audio& audio) {
    std::vector<float> mono(audio.frames());
    for (std::size_t frame = 0; frame < audio.frames(); ++frame)
        mono[frame] = audio.at(frame, 0);
    return largest_jump(mono);
}

// The period of a repeating wave, in frames, by autocorrelation: the lag the signal still looks
// like itself at. Harmonics make the waveform bumpy but do not change the period, which is why this
// works on a synth voice and would not work on, say, a snare drum.
int period_in_frames(const Audio& audio, std::size_t from, std::size_t frames) {
    std::vector<float> window;
    window.reserve(frames);
    float mean = 0.0f;
    for (std::size_t i = 0; i < frames; ++i) {
        window.push_back(audio.at(from + i, 0));
        mean += window.back();
    }
    mean /= static_cast<float>(frames);
    for (float& value : window)
        value -= mean;

    double best = -1.0;
    int best_lag = 0;
    for (int lag = 12; lag < static_cast<int>(frames) / 2; ++lag) {
        const std::size_t offset = static_cast<std::size_t>(lag);
        double sum = 0.0;
        for (std::size_t i = 0; i + offset < frames; ++i)
            sum += static_cast<double>(window[i]) * static_cast<double>(window[i + offset]);
        if (sum > best) {
            best = sum;
            best_lag = lag;
        }
    }
    return best_lag;
}

void note_on(Engine& engine, std::int64_t tick, int key, int velocity = 100) {
    REQUIRE(engine.push_event(Event::note_on(tick, 0, byte(key), byte(velocity))));
}

} // namespace

TEST_CASE("every engine parameter is one row of one table") {
    std::size_t count = 0;
    const ParamSpec* table = boreaz::engine::param_table(&count);
    REQUIRE(count > 10);

    Params defaults{};
    for (std::size_t row = 0; row < count; ++row) {
        const ParamSpec& spec = table[row];
        INFO("row " << row << ": " << (spec.key != nullptr ? spec.key : "(null)"));

        // A row has to be drawable and parseable: a name, a label a human wrote, a range.
        REQUIRE(spec.key != nullptr);
        REQUIRE(spec.label != nullptr);
        REQUIRE(spec.unit != nullptr);
        CHECK(spec.minimum < spec.maximum);
        CHECK(spec.minimum <= spec.default_value);
        CHECK(spec.default_value <= spec.maximum);

        // Exactly one kind of number. A row with neither is a parameter nobody can reach; a row
        // with both is a parameter whose value depends on which function looked first.
        CHECK((spec.real == nullptr) != (spec.integer == nullptr));

        // The default in the row is the default in the struct. Two defaults that disagree mean a
        // panel Reset lands somewhere the instrument never started.
        CHECK(boreaz::engine::param_read(spec, defaults) == doctest::Approx(spec.default_value).epsilon(0.001));

        // The key is the lookup, so a duplicate would shadow a parameter.
        CHECK(find(spec.key) == &spec);
    }

    CHECK(find("not_a_parameter") == nullptr);
}

TEST_CASE("the table is what a range means: clamping, sanitising, and the panel and the file agree") {
    Params params{};
    const ParamSpec* gain = find("master_gain_db");
    REQUIRE(gain != nullptr);
    boreaz::engine::param_write(*gain, 1000.0f, params);
    CHECK(params.master_gain_db == doctest::Approx(gain->maximum));
    boreaz::engine::param_write(*gain, -1000.0f, params);
    CHECK(params.master_gain_db == doctest::Approx(gain->minimum));

    // A NaN would poison every sample it touched, so it becomes the row's default instead of
    // surviving a clamp (std::clamp of a NaN is a NaN).
    Params broken{};
    broken.filter_cutoff_ratio = std::nanf("");
    broken.amp_sustain = std::numeric_limits<float>::infinity();
    broken.transpose_semitones = 9999;
    broken.sanitize();
    CHECK(broken.filter_cutoff_ratio == doctest::Approx(find("filter_cutoff_ratio")->default_value));
    CHECK(broken.amp_sustain ==
          doctest::Approx(find("amp_sustain")->default_value)); // the default, not zero
    CHECK(broken.transpose_semitones == 24);
    CHECK(std::isfinite(broken.master_gain_db));

    // The whole point of the row: the transpose cannot leave the keyboard, and the octave cannot
    // reach a note the tuning table does not have.
    Params wild{};
    wild.transpose_semitones = 999;
    wild.octave_shift = -999;
    wild.tuning_reference_hz = 100.0f;
    wild.clamp_all();
    CHECK(wild.transpose_semitones == 24);
    CHECK(wild.octave_shift == -4);
    CHECK(wild.tuning_reference_hz == doctest::Approx(415.0f).epsilon(0.001));
}

TEST_CASE("a parameter file refuses a number the engine cannot use, and says what it wanted") {
    const auto dir = std::filesystem::temp_directory_path() / "boreaz_param_table_tests";
    std::filesystem::create_directories(dir);

    const auto write_file = [](const std::filesystem::path& path, const std::string& body) {
        std::ofstream out(path);
        out << body;
    };

    Params loaded{};
    std::string error;
    const auto good = dir / "good.ini";
    write_file(good, "# a comment\nmaster_gain_db = -6.5\noctave_shift = -2\n");
    REQUIRE(load_params_file(good.string(), loaded, error));
    CHECK(loaded.master_gain_db == doctest::Approx(-6.5f));
    CHECK(loaded.octave_shift == -2);

    const auto hot = dir / "hot.ini";
    write_file(hot, "filter_cutoff_ratio = 1000\n");
    CHECK_FALSE(load_params_file(hot.string(), loaded, error));
    INFO("error: " << error);
    CHECK(error.find("filter_cutoff_ratio") != std::string::npos);
    CHECK(error.find("24") != std::string::npos); // it says what it wanted, not just "invalid"

    const auto typo = dir / "typo.ini";
    write_file(typo, "master_gian_db = -6\n");
    CHECK_FALSE(load_params_file(typo.string(), loaded, error));
    CHECK(error.find("master_gian_db") != std::string::npos);

    const auto fraction = dir / "fraction.ini";
    write_file(fraction, "octave_shift = 1.5\n");
    CHECK_FALSE(load_params_file(fraction.string(), loaded, error));
    CHECK(error.find("whole number") != std::string::npos);

    // The instrument's own defaults are a file like any other, and the app refuses to start when
    // they do not load, so the table has to agree with what ships.
    std::filesystem::path ini = std::filesystem::current_path() / "config" / "params.ini";
    if (!std::filesystem::is_regular_file(ini))
        ini = std::filesystem::path("..") / "config" / "params.ini";
    REQUIRE(std::filesystem::is_regular_file(ini));
    Params shipped{};
    REQUIRE_MESSAGE(load_params_file(ini.string(), shipped, error), ini.string() << ": " << error);
}

TEST_CASE("a parameter moves toward its new value instead of arriving there") {
    // Pure function: one block closes one block's worth of the gap and nothing more, and whole
    // numbers are not ramped, because a transpose of 3.5 is not a thing a player asked for.
    Params live{};
    Params target{};
    live.master_gain_db = 0.0f;
    target.master_gain_db = 12.0f;
    target.transpose_semitones = 3;
    boreaz::engine::param_slew(live, target, 0.1f);
    CHECK(live.master_gain_db == doctest::Approx(1.2f).epsilon(0.01));
    CHECK(live.transpose_semitones == 3);

    // It converges, and lands exactly.
    for (int block = 0; block < 400; ++block)
        boreaz::engine::param_slew(live, target, 0.1f);
    CHECK(live.master_gain_db == doctest::Approx(12.0f).epsilon(0.001));

    // alpha 1 is "no smoothing", which has to be the target itself so a caller can turn it off.
    Params snap{};
    boreaz::engine::param_slew(snap, target, 1.0f);
    CHECK(snap.master_gain_db == doctest::Approx(12.0f));
}

TEST_CASE("a cutoff slider thrown from one end of its range to the other does not click") {
    // The metric first: a genuine discontinuity has to be loud to this measure, or the test below
    // passes for the wrong reason.
    const std::vector<float> step(64, 0.0f);
    std::vector<float> clicked = step;
    for (std::size_t i = 32; i < clicked.size(); ++i)
        clicked[i] = 0.5f;
    CHECK(largest_jump(clicked) > 0.4f);

    auto at = [](float cutoff_ratio) {
        Params params{};
        params.master_gain_db = -6.0f;
        params.filter_resonance = 1.0f; // a resonant filter is where a step becomes a click
        params.filter_cutoff_ratio = cutoff_ratio;
        return params;
    };

    const float low = find("filter_cutoff_ratio")->minimum;
    const float high = find("filter_cutoff_ratio")->maximum;

    // The same note, already sitting at the end of the range: what its own waveform does per sample.
    Engine steady(at(high));
    steady.prepare(fixture::kRate);
    note_on(steady, 0, 60, 110);
    fixture::render(steady, 4096);
    const float steady_jump = largest_jump(fixture::render(steady, 4096));

    // ... and the same note with the slider thrown across its whole range halfway through.
    Engine thrown(at(low));
    thrown.prepare(fixture::kRate);
    note_on(thrown, 0, 60, 110);
    fixture::render(thrown, 4096);
    thrown.set_params(at(high));
    const Audio after = fixture::render(thrown, 4096);
    const float thrown_jump = largest_jump(after);

    // No click means the transition never moves the waveform more than the finished sound moves by
    // itself. Anything larger is a seam; a real discontinuity here is two orders of magnitude up.
    INFO("peak " << after.peak << ", jump while thrown " << thrown_jump << ", steady jump "
                 << steady_jump);
    REQUIRE(after.peak > fixture::kSilence); // it is still playing, or the comparison is meaningless
    REQUIRE(steady_jump > 0.0f);
    CHECK(thrown_jump <= steady_jump * 1.25f);
    CHECK(after.finite);
}

TEST_CASE("transpose retunes a held note, and cannot push it out of the range") {
    Params initial{};
    initial.master_gain_db = -6.0f;
    Engine engine(initial);
    engine.prepare(fixture::kRate);
    note_on(engine, 0, 60, 110);

    const Audio before = fixture::render(engine, 8192);
    const int period = period_in_frames(before, 2048, 4096);
    INFO("period at 60: " << period << " frames");
    REQUIRE(period > 12);

    Params up = initial;
    up.transpose_semitones = 12;
    engine.set_params(up);
    const Audio after = fixture::render(engine, 8192);
    const int up_period = period_in_frames(after, 2048, 4096);
    INFO("period after +12: " << up_period << " frames");

    // The note that was already sounding: an octave up is half the period. The autocorrelation
    // lands on a whole frame, so allow a frame either way rather than pretending otherwise.
    CHECK(up_period > 0);
    CHECK(up_period <= period / 2 + 1);
    CHECK(up_period >= period / 2 - 1);

    // And the note the engine reports moved with it, which is what the readout shows.
    CHECK(engine.sounding_note(60) == 72);
}

TEST_CASE("a voice retuned past the top of the range stays inside it") {
    Params params{};
    params.transpose_semitones = 0;
    SynthVoice voice;
    boreaz::engine::NoteRequest request;
    request.key = 108;
    request.note = 108;                       // C8, the top of the instrument
    request.velocity = 1.0f;
    request.frequency = 4186.01f;
    voice.start(request, params, fixture::kRate, 0);

    // +24 and +4 octaves: the pitch may not leave what the sample rate can hold, and the note
    // number may not leave the 128 entries the tuning table has.
    voice.retune(24, params);
    std::vector<float> left(256, 0.0f);
    std::vector<float> right(256, 0.0f);
    voice.render(left.data(), right.data(), 256);
    CHECK(voice.frequency() <= 0.49f * fixture::kRate);
    CHECK(voice.frequency() >= 1.0f);
    CHECK(voice.note() == 127);
    CHECK(std::isfinite(voice.frequency()));

    voice.retune(-60, params);
    CHECK(voice.note() == 67);
    CHECK(voice.frequency() >= 1.0f);

    for (float sample : left)
        CHECK(std::isfinite(sample));
}

TEST_CASE("a note that was never retuned is the note that was played") {
    // The retune must be a response to a change, not a thing that happens to the next note: a voice
    // started after the transpose is pitched once, from the transposed key, like every other note.
    Params initial{};
    initial.transpose_semitones = 12;
    Engine engine(initial);
    engine.prepare(fixture::kRate);
    note_on(engine, 0, 60, 110);

    const Audio audio = fixture::render(engine, 8192);
    const int period = period_in_frames(audio, 2048, 4096);
    INFO("period: " << period << " frames");
    // C5 (523 Hz) at 44.1 kHz is 84 frames. The un-transposed note would be 169.
    CHECK(period == doctest::Approx(84).epsilon(0.05));
    CHECK(engine.sounding_note(60) == 72);
}

TEST_CASE("--gain sets a parameter through the table, and refuses the way a file refuses") {
    // The flag exists because "it is too quiet" is discovered in the first ten seconds, and a
    // parameter file is not what you reach for then. It shares the parser, the table and the
    // refusals with `--config`, which is the only reason it cannot drift from the slider.
    Params params;
    std::string error;
    CHECK(boreaz::config::set_param_from_text("--gain", "master_gain_db", "-6.5", params, error));
    CHECK(params.master_gain_db == doctest::Approx(-6.5f));
    CHECK(error.empty());

    const ParamSpec* gain = find("master_gain_db");
    REQUIRE(gain != nullptr);
    CHECK_FALSE(boreaz::config::set_param_from_text("--gain", "master_gain_db", "1000", params,
                                                    error));
    INFO(error);
    // The message names the flag that was typed, not a line in a file nobody opened, and it names
    // the range the table owns.
    CHECK(error.find("--gain") != std::string::npos);
    CHECK(error.find("12") != std::string::npos);
    CHECK(params.master_gain_db == doctest::Approx(-6.5f)); // a refusal changes nothing

    CHECK_FALSE(boreaz::config::set_param_from_text("--gain", "master_gain_db", "loud", params,
                                                    error));
    CHECK(error.find("--gain") != std::string::npos);
    CHECK_FALSE(boreaz::config::set_param_from_text("--gain", "no_such_parameter", "1", params,
                                                    error));

    // An integer row goes through the same door, with the same rule about pretending.
    Params whole;
    CHECK_FALSE(boreaz::config::set_param_from_text("--transpose", "transpose_semitones", "1.5",
                                                    whole, error));
    CHECK(boreaz::config::set_param_from_text("--transpose", "transpose_semitones", "-2", whole,
                                              error));
    CHECK(whole.transpose_semitones == -2);
}
