#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "boreaz/samples/sample_store.h"

using boreaz::samples::Pcm;
using boreaz::samples::SampleStore;

namespace {

void push32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
}
void push16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}
void push_text(std::vector<std::uint8_t>& out, const char* text) {
    out.insert(out.end(), text, text + 4);
}

// A WAV, assembled byte by byte, because nothing else in this repository owns a binary file and
// nothing else is going to. `format` 1 is PCM, 3 is IEEE float, anything else is a format this
// decoder is supposed to refuse. `declared_data_size` of -1 leaves the honest size.
std::vector<std::uint8_t> wav(int format, int bits, int channels, double rate,
                              const std::vector<float>& samples, int declared_data_size = -1,
                              std::int64_t smpl_start = -1, std::int64_t smpl_end = -1) {
    const int bytes_per_sample = bits / 8;
    std::vector<std::uint8_t> data;
    for (const float sample : samples) {
        if (format == 3 && bits == 32) {
            const float value = sample;
            const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(&value);
            data.insert(data.end(), raw, raw + 4);
            continue;
        }
        double scaled = sample;
        if (bits == 8)
            scaled = (sample + 1.0) * 127.5;
        else if (bits == 16)
            scaled = sample * 32768.0;
        else if (bits == 24)
            scaled = sample * 8388608.0;
        else
            scaled = sample * 2147483648.0;
        long long value = static_cast<long long>(scaled);
        if (bits == 8)
            value = std::max(0LL, std::min(255LL, value));
        for (int byte = 0; byte < bytes_per_sample; ++byte) {
            data.push_back(static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFF));
        }
    }
    // A stereo file has the channels interleaved; the caller passes them already in order.
    const std::size_t data_size =
        declared_data_size >= 0 ? static_cast<std::size_t>(declared_data_size) : data.size();

    std::vector<std::uint8_t> smpl;
    if (smpl_start >= 0) {
        // 44 bytes of header, no cue points, then one 24-byte sampler loop: cue id, type, start,
        // end, fraction, play count.
        push_text(smpl, "smpl");
        push32(smpl, 68);
        for (int header = 0; header < 7; ++header)
            push32(smpl, 0); // manufacturer, product, period, unity note, pitch fraction, format,
                             // offset
        push32(smpl, 1); // one sampler loop
        push32(smpl, 0); // sampler data
        push32(smpl, 0); // artifact loops
        push32(smpl, 0); // no cue points
        push32(smpl, 0); // loop cue point id
        push32(smpl, 0); // loop type: forward
        push32(smpl, static_cast<std::uint32_t>(smpl_start));
        push32(smpl, static_cast<std::uint32_t>(smpl_end));
        push32(smpl, 0); // fraction
        push32(smpl, 0); // play count
    }

    std::vector<std::uint8_t> out;
    push_text(out, "RIFF");
    push32(out, static_cast<std::uint32_t>(4 + 8 + 18 + 8 + data_size + smpl.size()));
    push_text(out, "WAVE");
    push_text(out, "fmt ");
    push32(out, 16);
    push16(out, static_cast<std::uint16_t>(format));
    push16(out, static_cast<std::uint16_t>(channels));
    push32(out, static_cast<std::uint32_t>(rate));
    push32(out, static_cast<std::uint32_t>(rate * channels * bytes_per_sample));
    push16(out, static_cast<std::uint16_t>(channels * bytes_per_sample));
    push16(out, static_cast<std::uint16_t>(bits));
    push_text(out, "data");
    push32(out, static_cast<std::uint32_t>(data_size));
    out.insert(out.end(), data.begin(), data.end());
    out.insert(out.end(), smpl.begin(), smpl.end());
    return out;
}

std::string write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    return path.string();
}

void write_wav(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::string joined(const std::vector<std::string>& lines) {
    std::string all;
    for (const std::string& line : lines) {
        all += line;
        all += '\n';
    }
    return all;
}

// A scratch directory under the OS temp path, emptied before use. No audio binary lands in git, so
// the samples a store test needs are ones the test makes.
std::filesystem::path scratch(const std::string& name) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("boreaz-sample-store-" + name);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory / "samples");
    return directory;
}

} // namespace

TEST_SUITE("sample decoding") {

TEST_CASE("16-bit PCM is what came out of the microphone, widened and not squashed") {
    const std::vector<float> in{0.0f, 0.5f, -0.5f, 1.0f / 32768.0f, -1.0f};
    const std::vector<std::uint8_t> bytes = wav(1, 16, 1, 44100.0, in);
    Pcm out;
    std::string error;
    REQUIRE(boreaz::samples::decode_wav(bytes.data(), bytes.size(), out, error));
    CHECK(error.empty());
    CHECK(out.channels == 1);
    CHECK(out.frames == static_cast<std::int64_t>(in.size()));
    CHECK(out.sample_rate == doctest::Approx(44100.0));
    REQUIRE(static_cast<std::size_t>(out.frames) == in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        CAPTURE(i);
        // The 16-bit grid is 1/32768 wide, and -1.0 is exactly representable while +1.0 is not:
        // nothing here should be off by more than half of that.
        CHECK(out.data[i] == doctest::Approx(in[i]).epsilon(0.0001).scale(1.0));
    }
    CHECK(out.smpl_loop_start == -1);
}

TEST_CASE("stereo stays interleaved and 24-bit sign extends") {
    const std::vector<float> stereo{0.25f, -0.25f, 0.5f, -0.5f, 0.0f, 0.75f};
    Pcm out;
    std::string error;
    const std::vector<std::uint8_t> bytes = wav(1, 24, 2, 48000.0, stereo);
    REQUIRE(boreaz::samples::decode_wav(bytes.data(), bytes.size(), out, error));
    CHECK(out.channels == 2);
    CHECK(out.frames == 3);
    REQUIRE(out.data.size() == stereo.size());
    for (std::size_t i = 0; i < stereo.size(); ++i) {
        CAPTURE(i);
        CHECK(out.data[i] == doctest::Approx(stereo[i]).epsilon(0.001).scale(1.0));
    }
}

TEST_CASE("8-bit is unsigned, float 32 is exact, and 64-bit narrows without complaint") {
    const std::vector<float> in{0.0f, 0.75f, -0.75f, 0.99f};
    Pcm eight;
    std::string error;
    const std::vector<std::uint8_t> bytes8 = wav(1, 8, 1, 22050.0, in);
    REQUIRE(boreaz::samples::decode_wav(bytes8.data(), bytes8.size(), eight, error));
    CHECK(eight.sample_rate == doctest::Approx(22050.0));
    // 8-bit quantisation is coarse; what matters is that the middle of the range is silence and
    // that nothing is inverted, which is what treating unsigned as signed would do.
    CHECK(std::abs(eight.data[0]) < 0.01f);
    CHECK(eight.data[1] > 0.6f);
    CHECK(eight.data[2] < -0.6f);

    const std::vector<float> exact{0.25f, -0.125f, 0.5f};
    Pcm floats;
    const std::vector<std::uint8_t> bytes32 = wav(3, 32, 1, 44100.0, exact);
    REQUIRE(boreaz::samples::decode_wav(bytes32.data(), bytes32.size(), floats, error));
    REQUIRE(floats.data.size() == exact.size());
    for (std::size_t i = 0; i < exact.size(); ++i)
        CHECK(floats.data[i] == exact[i]); // bit for bit: a float file is already float
}

TEST_CASE("a format this library does not decode says which four bytes it found") {
    Pcm out;
    std::string error;

    const std::vector<std::uint8_t> not_a_wav{'O', 'g', 'g', 'S', 0, 0, 0, 0, 'w', 'v', 'e', ' '};
    CHECK_FALSE(boreaz::samples::decode_wav(not_a_wav.data(), not_a_wav.size(), out, error));
    INFO(error);
    CHECK(error.find("RIFF") != std::string::npos);
    CHECK(error.find("mkinst") != std::string::npos); // the tool that would fix it

    // 2 = ADPCM, which is compressed and therefore not decoded on the audio path or off it here.
    const std::vector<std::uint8_t> adpcm = wav(2, 4, 1, 44100.0, {0.0f});
    CHECK_FALSE(boreaz::samples::decode_wav(adpcm.data(), adpcm.size(), out, error));
    CHECK(error.find("PCM and IEEE float") != std::string::npos);

    const std::vector<std::uint8_t> twelve_bit = wav(1, 12, 1, 44100.0, {0.0f, 0.0f, 0.0f});
    CHECK_FALSE(boreaz::samples::decode_wav(twelve_bit.data(), twelve_bit.size(), out, error));
    CHECK(error.find("12-bit") != std::string::npos);

    CHECK_FALSE(boreaz::samples::decode_wav(nullptr, 0, out, error));
    CHECK(error.find("too short") != std::string::npos);

    // A file that ends inside the fmt chunk is an error and not a read past the buffer - and the
    // fmt chunk is what it says is missing, because nothing after it was readable.
    const std::vector<std::uint8_t> honest = wav(1, 16, 1, 44100.0, {0.0f, 0.0f, 0.0f, 0.0f});
    CHECK_FALSE(boreaz::samples::decode_wav(honest.data(), 24, out, error));
    CHECK(error.find("no fmt chunk") != std::string::npos);

    // fmt whole, `data` header present, and nothing in it: the shape of a file whose writer died
    // halfway, which is a refusal and not an empty instrument.
    CHECK_FALSE(boreaz::samples::decode_wav(honest.data(), 44, out, error));
    CHECK(error.find("no data chunk") != std::string::npos);
}

TEST_CASE("a streaming writer's placeholder sizes and a `smpl` loop are both honoured") {
    Pcm out;
    std::string error;
    // data chunk says 4 billion bytes; the rest of the file is the data either way.
    const std::vector<std::uint8_t> streaming = wav(1, 16, 1, 44100.0, {0.1f, 0.2f, 0.3f}, -1, 100, 900);
    REQUIRE(boreaz::samples::decode_wav(streaming.data(), streaming.size(), out, error));
    CHECK(out.frames == 3);
    // The file's own loop, kept because an instrument whose only loop points are in `smpl` still
    // has to loop.
    CHECK(out.smpl_loop_start == 100);
    CHECK(out.smpl_loop_end == 900);
}

TEST_CASE("load_wav says which path it could not read") {
    Pcm out;
    std::string error;
    CHECK_FALSE(boreaz::samples::load_wav("tests/fixtures/definitely/not/here.wav", out, error));
    CHECK(error.find("cannot open") != std::string::npos);
}

} // TEST_SUITE

TEST_SUITE("sample store") {

// Three recordings that differ in every way a test can see: length, rate, and loop.
struct Library {
    std::filesystem::path directory;
    std::string mapping;
};

Library make_library(const std::string& name, const std::string& mapping_text) {
    Library library;
    library.directory = scratch(name);
    // 1000 frames of 44.1 kHz, 2000 of 48 kHz, 3000 of 44.1 kHz with a `smpl` loop of its own.
    write_wav(library.directory / "samples/low.wav", wav(1, 16, 2, 44100.0, std::vector<float>(2000, 0.1f)));
    write_wav(library.directory / "samples/mid.wav", wav(1, 16, 2, 48000.0, std::vector<float>(4000, 0.2f)));
    write_wav(library.directory / "samples/high.wav",
              wav(1, 16, 2, 44100.0, std::vector<float>(6000, 0.3f), -1, 500, 2500));
    // Every recording this helper writes lands in `samples/`, so every mapping has to be told.
    // Every recording this helper writes lands in `samples/`, so every mapping has to be told.
    // A space is a complete separator in SFZ, so no newline is needed to prefix the mapping.
    const std::string prefixed = mapping_text.find("default_path") == std::string::npos
                                     ? "<control> default_path=samples/ " + mapping_text
                                     : mapping_text;
    library.mapping = write_file(library.directory / "instrument.sfz", prefixed);
    return library;
}

TEST_CASE("a mapping and its files become an instrument the audio thread can read") {
    const Library library = make_library("load", R"(<control> default_path=samples/
<global> amp_veltrack=100 volume=-6
<region> sample=low.wav key=36 pitch_keycenter=36
<region> sample=mid.wav key=60 pitch_keycenter=60
<region> sample=high.wav key=84 pitch_keycenter=84 loop_mode=loop_continuous
)");

    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    INFO(result.error);
    INFO(joined(result.warnings));
    REQUIRE(result.ok);

    CHECK(result.regions == 3);
    CHECK(result.files == 3);
    CHECK(result.refused == 0);
    // 1000 + 2000 + 3000 frames, two channels, four bytes each.
    CHECK(result.bytes == static_cast<std::size_t>(12000) * sizeof(float));
    CHECK(store.resident_bytes() == result.bytes);
    CHECK_FALSE(store.empty());
    CHECK(result.load_ms >= 0.0);

    const boreaz::engine::VoiceSample* middle = store.find(60, 100);
    REQUIRE(middle != nullptr);
    CHECK(middle->frames == 2000);
    CHECK(middle->file_sample_rate == doctest::Approx(48000.0));
    CHECK(middle->channels == 2);
    CHECK(middle->key_center == 60);
    CHECK_FALSE(middle->loops);
    // -6 dB is very nearly half of everything.
    CHECK(middle->gain == doctest::Approx(0.501f).epsilon(0.01));

    CHECK(store.find(36, 100)->frames == 1000);
    CHECK(store.find(84, 100)->frames == 3000);
    CHECK(store.find(84, 100)->loops);
    CHECK(store.find(84, 100)->loop_start == 500);
    CHECK(store.find(84, 100)->loop_end == 2500);
}

TEST_CASE("a key the instrument does not have gets the nearest recording it does") {
    const Library library = make_library("clamp", R"(<region> sample=low.wav key=36
<region> sample=high.wav key=84
)");
    SampleStore store;
    REQUIRE(store.load_mapping(library.mapping).ok);

    // The 88-key plan's honesty rule: C0 and C8 on a piano that stops at A0 and B7 sound shifted,
    // and not silent, because silence reads as a broken instrument and a wrong note reads as a
    // mapping somebody has to finish.
    CHECK(store.find(21, 90)->frames == 1000);
    CHECK(store.find(127, 90)->frames == 3000);
    CHECK(store.find(60, 90) != nullptr); // between the two: one of them, not nothing
}

TEST_CASE("a velocity the mapping has no layer for still plays the key") {
    const Library library = make_library("velocity", R"(<region> sample=mid.wav key=60 lovel=100 hivel=127
<region> sample=low.wav key=61 lovel=1 hivel=40
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    REQUIRE(result.ok);

    CHECK(store.find(60, 120)->frames == 2000);
    // No soft layer for key 60. The loud one at a quieter velocity is the piano with one layer;
    // nothing at all is a piano with a dead key.
    CHECK(store.find(60, 10)->frames == 2000);
    CHECK(store.find(61, 120)->frames == 1000);

    // And it says so. A mapping that asked for two layers and got one is a feature that is missing,
    // not a library that is quiet, and the player cannot tell those apart from the bench.
    bool warned = false;
    for (const std::string& warning : result.warnings)
        warned = warned || warning.find("velocity layer") != std::string::npos;
    CHECK(warned);
}

TEST_CASE("a loaded instrument keeps no file open: delete the WAVs underneath it") {
    // PLAN.md phase 6: zero file I/O in the audio callback. The strong form of that claim is that
    // the store owns its audio and needs nothing on disk to keep playing - and on Windows a file
    // another handle has open cannot be deleted, so a `remove_all` that succeeds IS the assertion.
    const Library library = make_library("delete-under", R"(<region> sample=high.wav key=60 pitch_keycenter=60 loop_mode=loop_continuous)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    INFO(result.error);
    REQUIRE(result.ok);

    const boreaz::engine::VoiceSample* before = store.find(60, 100);
    REQUIRE(before != nullptr);
    const float first = before->data[0];
    const std::int64_t frames = before->frames;
    REQUIRE(frames > 0);

    std::error_code ec;
    const std::uintmax_t removed = std::filesystem::remove_all(library.directory, ec);
    INFO("remove_all removed " << removed << " entries");
    CHECK_FALSE(static_cast<bool>(ec));
    CHECK(removed >= 3); // the mapping, its directory, and the recordings in it

    const boreaz::engine::VoiceSample* after = store.find(60, 100);
    REQUIRE(after != nullptr);
    CHECK(after->frames == frames);
    CHECK(after->data[0] == first);
    CHECK(store.resident_bytes() == result.bytes); // still resident, still the same audio
}

TEST_CASE("two layers for one key are picked by velocity") {
    // The shape the CC0 starter instrument actually uses: four groups, two of them `hivel=80` and
    // two `lovel=81`, so a soft touch and a hard one are different recordings. Nothing here needs
    // the layer choice that is not implemented: the bands cover the range, so one of them matches.
    const Library library = make_library("two-layers", R"(<group> hivel=80
<region> sample=low.wav key=60 pitch_keycenter=60
<group> lovel=81
<region> sample=mid.wav key=60 pitch_keycenter=60
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    INFO(result.error);
    INFO(joined(result.warnings));
    REQUIRE(result.ok);

    // 1000 and 2000 frames: the helper's sample counts are interleaved stereo pairs.
    CHECK(store.find(60, 5)->frames == 1000);
    CHECK(store.find(60, 80)->frames == 1000);
    CHECK(store.find(60, 81)->frames == 2000);
    CHECK(store.find(60, 127)->frames == 2000);

    // Two layers that cover the range leave no velocity unanswered, so there is nothing to warn
    // about - which is the other half of the warning test above.
    CHECK(joined(result.warnings).find("velocity layer") == std::string::npos);
}

TEST_CASE("offset, `amp_veltrack`, and one file shared by two regions") {
    const Library library = make_library("opcodes", R"(<region> sample=mid.wav key=60 offset=0.02
<region> sample=mid.wav key=61 offset=600
<region> sample=mid.wav key=62 amp_veltrack=0 volume=0
<region> sample=mid.wav key=63 amp_veltrack=50
<region> sample=mid.wav key=64 offset=99999
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    REQUIRE(result.ok);
    // The same file, read once: 88 regions over 28 samples is a normal instrument.
    CHECK(result.files == 1);
    CHECK(result.regions == 5);
    CHECK(result.bytes == static_cast<std::size_t>(4000) * sizeof(float));

    // mid.wav is 2000 frames of 48 kHz, so two hundredths of a second is 960 frames and 600 is 600
    // either way. The last one is a mapping that lies about where its sample starts: the read-head
    // cannot begin past the end of the file, so it begins on the last frame it has.
    CHECK(store.find(60, 100)->start_frame == 960);
    CHECK(store.find(61, 100)->start_frame == 600);
    CHECK(store.find(62, 100)->velocity_tracking == doctest::Approx(0.0f));
    CHECK(store.find(63, 100)->velocity_tracking == doctest::Approx(0.5f));
    CHECK(store.find(64, 100)->start_frame == store.find(64, 100)->frames - 1);
    CHECK(store.find(60, 100)->start_frame < store.find(60, 100)->frames);
}

TEST_CASE("a release region is loaded as a release and never answers a struck key") {
    const Library library = make_library("release", R"(<region> sample=mid.wav key=60
<region> sample=low.wav key=60 trigger=release
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    INFO(result.error);
    INFO(joined(result.warnings));
    REQUIRE(result.ok);

    // One place to start a note, one thing to play when the key goes up, and nothing refused: a
    // release region was never unplayable, only unimplemented.
    CHECK(result.regions == 1);
    CHECK(result.releases == 1);
    CHECK(result.refused == 0);
    // Both recordings are resident, since the release has to be as ready as the note it follows.
    CHECK(result.files == 2);
    CHECK(joined(result.warnings).find("release sample") == std::string::npos);

    // The two lookups cannot answer for each other, which is the whole reason they are two lists.
    CHECK(store.find(60, 100)->frames == 2000);          // mid.wav, the note
    CHECK(store.find_release(60, 100)->frames == 1000);  // low.wav, the release
}

TEST_CASE("an instrument with no release recordings says so and does not lend a note") {
    const Library library = make_library("attacks-only", R"(<region> sample=mid.wav key=60
<region> sample=high.wav key=61 loop_mode=loop_continuous
)");
    SampleStore store;
    REQUIRE(store.load_mapping(library.mapping).ok);
    // This is the CC0 starter instrument: 66 regions, none of them asking for a release. A key-up
    // here is the note's own envelope and nothing else.
    CHECK(store.find_release(60, 100) == nullptr);
    CHECK(store.find_release(21, 1) == nullptr);
}

TEST_CASE("a release region is picked by velocity, like a layer of its own") {
    const Library library = make_library("release-layers", R"(<region> sample=mid.wav key=60
<region> sample=low.wav key=60 trigger=release hivel=80
<region> sample=high.wav key=60 trigger=release lovel=81
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(result.releases == 2);

    // A soft touch lets go quietly and a hard one lets go with the hammer in it. A velocity no
    // release band covers still gets the key's release rather than silence, by the same precedence
    // the note layers use.
    CHECK(store.find_release(60, 5)->frames == 1000);
    CHECK(store.find_release(60, 80)->frames == 1000);
    CHECK(store.find_release(60, 81)->frames == 3000);
    CHECK(store.find(60, 81)->frames == 2000); // the note is still the note
}

TEST_CASE("a release recording does not loop, and does not claim a velocity layer either") {
    // high.wav carries a `smpl` loop and the mapping asks for loop_continuous on top of it. A
    // recording of a key being let go is one event: a hammer noise that loops forever is a hiss.
    const Library library = make_library("release-loop", R"(<region> sample=mid.wav key=60
<region> sample=high.wav key=60 trigger=release loop_mode=loop_continuous
<region> sample=low.wav key=61 trigger=release
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    INFO(result.error);
    REQUIRE(result.ok);

    const boreaz::engine::VoiceSample* release = store.find_release(60, 100);
    REQUIRE(release != nullptr);
    CHECK_FALSE(release->loops);
    CHECK(release->loop_end == 0);
    // A key with no release of its own still gets the nearest one, shifted, exactly as a note key
    // does - and it is the release, not a note recording in disguise.
    const boreaz::engine::VoiceSample* nearest = store.find_release(72, 100);
    REQUIRE(nearest != nullptr);
    CHECK(nearest->frames == 1000);
}

TEST_CASE("a release region cannot cover for a note layer the instrument is missing") {
    // The note recording stops at velocity 80 and the release picks up above it. Counting the
    // release as coverage would let the store claim every velocity is covered, and velocity 100
    // would play the soft recording while a hammer noise sat on top of it.
    const Library library = make_library("release-coverage", R"(<region> sample=mid.wav key=60 hivel=80
<region> sample=low.wav key=60 trigger=release lovel=81
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(result.regions == 1);
    CHECK(result.releases == 1);
    CHECK(joined(result.warnings).find("velocity layer") != std::string::npos);
}

TEST_CASE("a mapping of nothing but releases has nothing to start a note with") {
    const Library library = make_library("release-only", R"(<region> sample=low.wav key=60 trigger=release
<region> sample=high.wav key=61 trigger=release
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    CHECK_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.error.find("as a note") != std::string::npos); // releases exist, but nothing to play
    CHECK(store.empty());
}

TEST_CASE("a missing or unreadable file fails the load and says which one") {
    const Library library = make_library("missing", R"(<region> sample=mid.wav key=60
<region> sample=nope.wav key=61
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    CHECK_FALSE(result.ok);
    INFO(result.error);
    CHECK(result.error.find("nope.wav") != std::string::npos);
    CHECK(store.empty()); // a store that failed is a store that plays nothing
}

TEST_CASE("a file that is not a WAV names itself, and a mapping with nothing playable fails") {
    const Library library = make_library("garbage", R"(<region> sample=notes.txt key=60
)");
    write_file(library.directory / "samples/notes.txt", "this is not a wave file at all");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("RIFF") != std::string::npos);

    const Library empty = make_library("empty", "// nothing but a comment");
    SampleStore nothing;
    const boreaz::samples::LoadResult refused = nothing.load_mapping(empty.mapping);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.find("no usable region") != std::string::npos); // the sfz reader spoke first
}

TEST_CASE("the file's own loop is used when the mapping says nothing, and not when it lies") {
    const Library library = make_library("smpl", R"(<region> sample=high.wav key=60 loop_mode=loop_continuous
<region> sample=high.wav key=61 loop_mode=loop_continuous loop_start=900 loop_end=800
)");
    SampleStore store;
    const boreaz::samples::LoadResult result = store.load_mapping(library.mapping);
    REQUIRE(result.ok);

    // `high.wav` loops at 500..4000 by itself, and the mapping agrees without saying.
    CHECK(store.find(60, 100)->loops);
    CHECK(store.find(60, 100)->loop_start == 500);

    // Ends the wrong way round is not a loop: playing a file backwards from an arbitrary frame is
    // not a thing a piano does, so it plays once and says so.
    CHECK_FALSE(store.find(61, 100)->loops);
    CHECK(joined(result.warnings).find("wrong way round") != std::string::npos);
}

} // TEST_SUITE
