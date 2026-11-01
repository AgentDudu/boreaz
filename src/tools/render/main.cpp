// boreaz-render: a Standard MIDI File and a parameter file in, one WAV file out, no device and
// no window. This is the tool that makes the engine's behaviour observable in CI - a render is a
// file, a file has a checksum, and a checksum cannot say "it sounded fine to me".
//
// It streams: one block of floats at a time, so a 40 minute song costs the same memory as a
// 4 second one, and it never holds the whole render in memory.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <psapi.h>
#endif

#include "boreaz/config/params_file.h"
#include "boreaz/engine/engine.h"
#include "boreaz/engine/param_table.h"
#include "boreaz/midifile/midifile.h"
#include "boreaz/samples/sample_store.h"

namespace {

using boreaz::config::load_params_file;
using boreaz::config::set_param_from_text;
using boreaz::engine::Engine;
using boreaz::engine::Params;

constexpr double kDefaultRate = 44100.0;
// 256 frames: under the engine's 512 frame scratch limit, and small enough that a render of any
// length costs the same handful of kilobytes.
constexpr int kBlock = 256;

// PLAN's real-time promise - "zero file I/O or page faults in the callback" - has been written
// down since phase 1 and counted once, ever. These are the process counters that say whether it is
// true, read around the engine call and nothing else, on the thread that makes the sound, with
// nothing else running: so a fault or a read inside the window belongs to the engine.
struct Counters {
    std::uint64_t faults = 0;
    std::uint64_t reads = 0;
    std::uint64_t writes = 0;

    Counters& operator+=(const Counters& other) {
        faults += other.faults;
        reads += other.reads;
        writes += other.writes;
        return *this;
    }
};

Counters counters_now() {
#ifdef _WIN32
    Counters out;
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        out.faults = memory.PageFaultCount;
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
        out.reads = io.ReadOperationCount;
        out.writes = io.WriteOperationCount;
    }
    return out;
#else
    return {};
#endif
}

Counters operator-(const Counters& a, const Counters& b) {
    return {a.faults - b.faults, a.reads - b.reads, a.writes - b.writes};
}

// Blocks whose faults do not count against the claim: the first touch of the voice pool, the
// scratch, and the song is a warm-up cost paid once, and the promise is about steady state.
constexpr int kWarmBlocks = 32;

// Printing zeros on a platform with no counters would be a claim, not a measurement.
#ifdef _WIN32
constexpr bool kCountersMeasured = true;
#else
constexpr bool kCountersMeasured = false;
#endif

constexpr int kOk = 0;
constexpr int kUsage = 2;
constexpr int kInputFailed = 3;
constexpr int kOutputFailed = 4;

struct Options {
    std::string input;
    std::string output;
    std::string params_path; // optional
    // "--gain DB" because measuring a level needs a way to ask for another one without writing a
    // file first. Text, not a float: the parameter table owns the range and the message.
    std::string gain;
    // Render the sample library instead of the synthesiser, so the thing a listening test hears is
    // the thing the app would play. No file, no download, no driver: the mapping and its WAVs are
    // enough, which is why the sampled path can be checked with a rendered file.
    std::string instrument;
    double rate = kDefaultRate;
    bool golden = false; // print a golden line instead of writing a file
    // Count page faults and file reads around the engine call. Off by default because the counters
    // are two syscalls per block, which is the thing a timing run would object to.
    bool counters = false;
};

void usage() {
    std::printf(
        "boreaz-render - a MIDI file in, a WAV file out\n"
        "\n"
        "usage: boreaz-render IN.mid OUT.wav [PARAMS] [--rate N]\n"
        "       boreaz-render --golden IN.mid [--rate N] [PARAMS]\n"
        "\n"
        "  --golden    render, then print one line of measurements instead of a file. The\n"
        "              committed goldens are regenerated with:\n"
        "                for f in tests/fixtures/midifile/*.mid; do\n"
        "                  build/boreaz-render --golden \"$f\"\ndone > tests/golden/midifile.txt\n"
        "  IN.mid      format 0 or 1, ticks per quarter note (see --help of boreaz for the\n"
        "              subset, or rather: boreaz-midifile refuses what it cannot honour)\n"
        "  OUT.wav     16 bit PCM, stereo, overwritten if it exists\n"
        "  PARAMS      optional 'key = value' file, one per line, # for comments.\n");
    // The keys are printed out of the table rather than listed here. A second copy of the list is
    // what has to be remembered when a parameter is added, and this one had already been forgotten:
    // the text that used to sit here named 17 keys while the table held 18.
    std::printf("              Keys are the engine parameters:");
    int column = 45; // the column the colon ends on, so the wrapping below knows where it is
    std::size_t rows = 0;
    const boreaz::engine::ParamSpec* table = boreaz::engine::param_table(&rows);
    for (std::size_t i = 0; i < rows; ++i) {
        std::string item(table[i].key);
        if (i + 1 < rows)
            item += ',';
        if (column + 1 + static_cast<int>(item.size()) >
            79) { // 79: what the rest of the text fits in
            std::printf("\n              ");
            column = 14;
        } else {
            std::printf(" ");
            ++column;
        }
        std::printf("%s", item.c_str());
        column += static_cast<int>(item.size());
    }
    std::printf("\n");
    std::printf("  --rate N    output sample rate in Hz (default 44100)\n"
                "  --gain DB   master output level in dB, -72 to +12, instead of the PARAMS file's "
                "master_gain_db\n"
                "  --instrument FILE  play a recorded instrument instead of the synthesiser, so a\n"
                "              rendered WAV is the audio the app would have made\n"
                "  --counters  report page faults and file reads around the engine call: the load,\n"
                "              the first 32 blocks, and every block after them. Zero reads and zero\n"
                "              faults in the last line is the real-time promise, measured\n"
                "\n"
                "Exit status: 0 ok, 2 bad usage, 3 input unreadable, 4 output unwritable.\n");
}

bool parse_arguments(int argc, char** argv, Options& options, std::string& error) {
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--counters") {
            options.counters = true;
            continue;
        }
        if (argument == "--rate") {
            if (i + 1 >= argc) {
                error = "--rate needs a value";
                return false;
            }
            const std::string value = argv[++i];
            // strtod rather than sscanf: MSVC's headers mark sscanf deprecated and clang-cl turns
            // that deprecation into an error under -Werror. The end pointer is stricter than the
            // count sscanf returned, which is a favour - "44100Hz" used to parse - and isfinite is
            // for "nan" and "inf", which strtod hands back happily and which compare false against
            // both ends of a range, so `--rate nan` used to be accepted.
            char* end = nullptr;
            const double rate = std::strtod(value.c_str(), &end);
            if (*end != '\0' || !std::isfinite(rate) || rate < 3000.0 || rate > 384000.0) {
                error = "--rate wants 3000 to 384000 Hz, got '" + value + "'";
                return false;
            }
            options.rate = rate;
            continue;
        }
        if (argument == "--instrument") {
            if (i + 1 >= argc) {
                error = "--instrument needs a value";
                return false;
            }
            options.instrument = argv[++i];
            continue;
        }
        if (argument == "--gain") {
            if (i + 1 >= argc) {
                error = "--gain needs a value";
                return false;
            }
            options.gain = argv[++i];
            continue;
        }
        if (argument == "-h" || argument == "--help") {
            usage();
            std::exit(kOk); // NOLINT: a tool exiting on --help is the tool working
        }
        if (argument == "--golden") {
            options.golden = true;
            continue;
        }
        if (argument.rfind("--", 0) == 0) {
            error = "unknown option '" + argument + "'";
            return false;
        }
        positional.push_back(argument);
    }
    // A parameter file is allowed here too, and is the second name rather than the third: every
    // A/B worth measuring is a parameter change, and a --golden that could only ever measure the
    // defaults could not have found the drive's harmonics, the room's level, or anything else that
    // only exists when a slider has moved.
    if (options.golden && positional.size() > 2) {
        error = "--golden takes an input MIDI file, an optional parameter file, and no output file";
        return false;
    }
    if (!options.golden && (positional.size() < 2 || positional.size() > 3)) {
        error = positional.empty() ? "an input MIDI file and an output WAV file are required"
                                   : "expected IN.mid OUT.wav and at most one parameter file";
        return false;
    }
    options.input = positional[0];
    // Only --golden may be called with one name, and reading positional[1] there is reading past
    // the end of the vector: the tool died with std::length_error before printing anything.
    if (positional.size() >= 2 && !options.golden)
        options.output = positional[1];
    const std::size_t params_at = options.golden ? 1u : 2u;
    if (positional.size() == params_at + 1u)
        options.params_path = positional[params_at];
    return true;
}

// ---------------------------------------------------------------- parameters
//
// In boreaz::config, because the app opens the same files against the same field table, and a key
// that exists in one copy of that table and not the other is a preset that renders differently from
// how it plays.

// ---------------------------------------------------------------- wav
//
// Canonical 16 bit PCM stereo, written by hand: 44 bytes of header is not worth a dependency,
// and an offline tool that gets its own output format wrong has nowhere to hide.
void put_u16(std::ostream& out, std::uint16_t value) {
    const char bytes[2] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
    out.write(bytes, 2);
}

void put_u32(std::ostream& out, std::uint32_t value) {
    char bytes[4];
    for (int i = 0; i < 4; ++i)
        bytes[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    out.write(bytes, 4);
}

bool write_wav_header(std::ofstream& out, double rate, std::uint32_t frames) {
    const std::uint32_t sample_rate = static_cast<std::uint32_t>(rate);
    if (sample_rate == 0)
        return false;
    const std::uint32_t bytes_per_second = sample_rate * 4u; // 2 channels, 16 bit

    out.write("RIFF", 4);
    put_u32(out, 44u - 8u + frames * 4u); // the whole file minus this field and the size field
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    put_u32(out, 16); // the canonical PCM format chunk
    put_u16(out, 1);  // 1 is PCM, not a codec we would have to name
    put_u16(out, 2);  // the engine is stereo out
    put_u32(out, sample_rate);
    put_u32(out, bytes_per_second);
    put_u16(out, 4); // one frame of both channels
    put_u16(out, 16);
    out.write("data", 4);
    put_u32(out, frames * 4u);
    return static_cast<bool>(out);
}

// The two size fields, now that the data exists.
void patch_sizes(std::ofstream& out, std::uint32_t frames) {
    out.seekp(4);
    put_u32(out, 44u - 8u + frames * 4u);
    out.seekp(40);
    put_u32(out, frames * 4u);
    out.seekp(0, std::ios::end);
}

// FNV-1a, 64 bit: not for anything adversarial, only for noticing that a render moved. A golden
// line of checksum, peak and steal count tells a reviewer that the synthesis changed and roughly
// how, which is more than a wall of audio bytes would say in a diff.
void fnv(std::uint64_t& hash, const std::int16_t* samples, std::size_t count) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(samples);
    for (std::size_t i = 0; i < count * sizeof(std::int16_t); ++i) {
        hash ^= bytes[i];
        hash *= 0x00000100000001B3ull;
    }
}

std::int16_t to_pcm(float sample) {
    // The engine promises +/-1.0, so this clamp is a seatbelt rather than a limiter: it exists so
    // a hypothetical 1.0000001 from a rounding path cannot wrap to -32768 and pop.
    const double scaled = std::clamp(sample, -1.0f, 1.0f) * 32768.0;
    return static_cast<std::int16_t>(std::lround(std::max(-32768.0, std::min(32767.0, scaled))));
}

double decibels(float peak) {
    if (!(peak > 0.0f))
        return -120.0;
    return 20.0 * std::log10(static_cast<double>(peak));
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parse_arguments(argc, argv, options, error)) {
        std::fprintf(stderr, "boreaz-render: %s (try --help)\n", error.c_str());
        return kUsage;
    }

    Params params;
    if (!options.params_path.empty() && !load_params_file(options.params_path, params, error)) {
        std::fprintf(stderr, "boreaz-render: %s\n", error.c_str());
        return kUsage;
    }
    // After the file, so the flag is the newest thing said about the level.
    if (!options.gain.empty() &&
        !set_param_from_text("--gain", "master_gain_db", options.gain, params, error)) {
        std::fprintf(stderr, "boreaz-render: %s\n", error.c_str());
        return kUsage;
    }
    params.clamp_all(); // a parameter file may ask for nonsense, and nonsense gets clamped loudly
    params.sanitize();

    boreaz::midifile::LoadResult loaded = boreaz::midifile::load_file(options.input, options.rate);
    if (!loaded.ok) {
        std::fprintf(stderr, "boreaz-render: %s\n", loaded.error.c_str());
        return kInputFailed;
    }
    for (const std::string& warning : loaded.warnings)
        std::fprintf(stderr, "boreaz-render: %s: %s\n", options.input.c_str(), warning.c_str());

    const boreaz::midifile::Song& song = loaded.song;
    const std::int64_t frames = boreaz::midifile::render_length(song);
    if (frames > static_cast<std::int64_t>(0xFFFFFFFFu)) {
        std::fprintf(stderr,
                     "boreaz-render: that render would be %lld seconds, which does not fit the "
                     "WAV format's 32 bit sizes\n",
                     static_cast<long long>(frames / static_cast<std::int64_t>(options.rate)));
        return kOutputFailed;
    }

    std::ofstream out;
    std::uint64_t hash = 0xCBF29CE484222325ull;
    if (!options.golden) {
        out.open(options.output, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "boreaz-render: cannot write '%s'\n", options.output.c_str());
            return kOutputFailed;
        }
        if (!write_wav_header(out, options.rate, static_cast<std::uint32_t>(frames))) {
            std::fprintf(stderr, "boreaz-render: writing '%s' failed part way through\n",
                         options.output.c_str());
            return kOutputFailed;
        }
    }

    // Before the engine, and alive longer than it: a voice holds a pointer into the store for as
    // long as it is sounding.
    boreaz::samples::SampleStore store;
    Engine engine(params);
    const Counters load_start = options.counters ? counters_now() : Counters{};
    if (!options.instrument.empty()) {
        const boreaz::samples::LoadResult sampled = store.load_mapping(options.instrument);
        if (!sampled.ok) {
            std::fprintf(stderr, "boreaz-render: %s\n", sampled.error.c_str());
            return kInputFailed;
        }
        engine.set_instrument(&store);
    }
    engine.prepare(options.rate);
    const Counters load_cost = options.counters ? counters_now() - load_start : Counters{};

    std::vector<float> block(static_cast<std::size_t>(kBlock) * 2u, 0.0f);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(kBlock) * 2u, 0);
    std::size_t cursor = 0;
    std::size_t dropped = 0;
    float peak = 0.0f;
    int loudest_voice_count = 0;
    std::int64_t written = 0;
    Counters warm;
    Counters steady;
    int block_index = 0;
    // Engine wall time only - the WAV write and the golden hash are deliberately outside these
    // windows, because the number a CPU claim is argued from has to be the callback's cost and
    // nothing else. Two clock reads per block against 5 ms of work is not a measurement error.
    std::uint64_t engine_ns = 0;

    while (written < frames) {
        const int chunk = static_cast<int>(
            std::min<std::int64_t>(kBlock, static_cast<std::int64_t>(block.size() / 2u)));
        const std::int64_t want =
            static_cast<std::int64_t>(std::min<std::int64_t>(chunk, frames - written));
        const Counters before = options.counters ? counters_now() : Counters{};
        const auto tick = std::chrono::steady_clock::now();
        const boreaz::midifile::FeedResult fed = boreaz::midifile::feed_and_render(
            engine, song, cursor, block.data(), static_cast<int>(want));
        engine_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 tick)
                .count());
        if (options.counters) {
            const Counters cost = counters_now() - before;
            (block_index++ < kWarmBlocks ? warm : steady) += cost;
        }
        cursor = fed.cursor;
        dropped += fed.dropped;
        for (std::int64_t i = 0; i < want * 2; ++i) {
            const float sample = block[static_cast<std::size_t>(i)];
            peak = std::max(peak, std::abs(sample));
            pcm[static_cast<std::size_t>(i)] = to_pcm(sample);
        }
        if (options.golden)
            fnv(hash, pcm.data(), static_cast<std::size_t>(want * 2));
        else
            out.write(reinterpret_cast<const char*>(pcm.data()),
                      static_cast<std::streamsize>(
                          want * 2 * static_cast<std::int64_t>(sizeof(std::int16_t))));
        written += want;
        loudest_voice_count = std::max(loudest_voice_count, engine.active_voices());
    }

    if (options.counters) {
        if (!kCountersMeasured) {
            std::printf("  counters         not measured on this platform\n");
        } else {
            const auto u = [](std::uint64_t v) { return static_cast<unsigned long long>(v); };
            std::printf("  counters         page faults / file reads / file writes around the engine\n");
            std::printf("    loading          %llu / %llu / %llu\n", u(load_cost.faults),
                        u(load_cost.reads), u(load_cost.writes));
            std::printf("    first %d blocks  %llu / %llu / %llu\n", kWarmBlocks, u(warm.faults),
                        u(warm.reads), u(warm.writes));
            std::printf("    after warm-up    %llu / %llu / %llu   <- zero here is the promise\n",
                        u(steady.faults), u(steady.reads), u(steady.writes));
        }
    }

    // What the engine cost, said as a fraction of one core, which is the unit every CPU claim in
    // PLAN is written in. The wall clock is taken around the engine call only, so this is the
    // callback's work: no sample loading, no WAV write, no hash, no driver and no sleeping. A
    // render runs as fast as it can, so engine time / audio time is the load one core would carry
    // if this were the audio thread. It goes to stderr in both modes, like the warnings: stdout is
    // the tool's report, and a regenerated golden must not be able to pick up a measurement.
    const double audio_seconds = static_cast<double>(frames) / options.rate;
    const double engine_seconds = static_cast<double>(engine_ns) * 1e-9;
    const double core_fraction = audio_seconds > 0.0 ? engine_seconds / audio_seconds : 0.0;
    const double speed = engine_seconds > 0.0 ? audio_seconds / engine_seconds : 0.0;
    std::fprintf(stderr,
                 "  engine work      %llu ms for %.2f s of audio = %.2f%% of one core (%.1fx "
                 "realtime), %d voice(s) at the loudest moment\n",
                 static_cast<unsigned long long>(engine_ns / 1000000u), audio_seconds,
                 core_fraction * 100.0, speed, loudest_voice_count);

    if (options.golden) {
        // A basename, so a golden never carries this machine's paths into the repository.
        const std::string name = std::filesystem::path(options.input).filename().string();
        std::printf("name=%s rate=%.0f frames=%lld fnv1a64=%016llx peak=%.6f steals=%d\n",
                    name.c_str(), options.rate, static_cast<long long>(frames),
                    static_cast<unsigned long long>(hash), static_cast<double>(peak),
                    engine.steal_count());
        return kOk;
    }

    if (!out) {
        std::fprintf(stderr, "boreaz-render: writing '%s' failed\n", options.output.c_str());
        return kOutputFailed;
    }
    patch_sizes(out, static_cast<std::uint32_t>(frames));
    out.close();

    std::printf("%s -> %s\n", options.input.c_str(), options.output.c_str());
    std::printf("  source           format %d, %d ticks per quarter note, %zu event(s), %zu "
                "tempo change(s)\n",
                song.format, song.ticks_per_quarter, song.events.size(), song.tempos.size());
    std::printf("  output           %lld frames at %.0f Hz, 16 bit stereo, %.2f s\n",
                static_cast<long long>(frames), options.rate,
                static_cast<double>(frames) / options.rate);
    std::printf("  peak             %.1f dBFS (%.4f), loudest moment %d voice(s)\n", decibels(peak),
                static_cast<double>(peak), loudest_voice_count);
    if (engine.steal_count() > 0)
        std::printf(
            "  voice stealing   %d voice(s) were stolen: the file wants more than %d notes\n",
            engine.steal_count(), Engine::kMaxPolyphony);
    if (dropped > 0)
        std::printf("  lost events      %zu could not be queued, so they are not in this file\n",
                    dropped);
    return kOk;
}
