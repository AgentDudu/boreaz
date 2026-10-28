// Boreaz - 88-key audio/MIDI synthesizer.
//
// This phase is the first one that makes sound: it opens an output, plays the engine on the
// audio thread, and takes notes from a MIDI keyboard. Deliberately still no window and no GUI -
// `--demo` and `--report` exist so "no audible glitch" and "under 3% of one core" can be checked
// from a terminal, which is also how they get checked on a machine with no sound card.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <miniaudio.h>

#include "boreaz/engine/engine.h"
#include "boreaz/engine/event.h"
#include "boreaz/io/audio_device.h"
#include "boreaz/io/midi_input.h"
#include "boreaz/version.h"

namespace {

using boreaz::engine::Engine;
using boreaz::engine::Event;
using boreaz::io::AudioDevice;
using boreaz::io::MidiInput;

// Exit statuses, so a script can tell "no keyboard" from "no sound card".
constexpr int kOk = 0;
constexpr int kBadUsage = 2;
constexpr int kAudioFailed = 3;
constexpr int kMidiFailed = 4;

struct Options {
    std::string device;          // empty means the system default output
    std::string midi;            // empty means the first input, if there is one
    bool list = false;
    bool demo = false;
    bool report = false;
    int period_frames = 512;
    int chord = 0; // with --demo: hold N notes for the whole run, to measure a load
    double seconds = 5.0;
};

void print_version() {
    int major = 0;
    int minor = 0;
    int patch = 0;
    glfwGetVersion(&major, &minor, &patch);

    std::printf("boreaz %s\n", boreaz::kVersionString);
    std::printf("  glfw      %d.%d.%d  %s\n", major, minor, patch, glfwGetVersionString());
    std::printf("  imgui     %s\n", ImGui::GetVersion());
    std::printf("  miniaudio %s\n", MA_VERSION_STRING);
}

void print_usage() {
    std::printf(
        "boreaz %s - 88-key audio/MIDI synthesizer\n"
        "\n"
        "usage: boreaz [option]...\n"
        "\n"
        "  -v, --version          print versions and exit\n"
        "  -h, --help             print this help and exit\n"
        "      --list             list audio outputs and MIDI inputs, then exit\n"
        "      --device NAME      output to use: index from --list, or part of its name\n"
        "      --midi NAME        MIDI input to use: index from --list, or part of its name\n"
        "      --latency FRAMES   requested buffer period in frames (default 512)\n"
        "      --demo             play a written part instead of waiting for a keyboard\n"
        "      --chord N          with --demo, hold N notes (1-16) for the whole run instead\n"
        "      --seconds N        with --demo, how long to run (default 5)\n"
        "      --report           print callback and CPU counters before exiting\n"
        "\n"
        "With no --midi and no --demo this opens the default output, waits for a keyboard, and\n"
        "stops when you press Enter. Exit status: 0 ok, 2 bad usage, 3 no audio, 4 MIDI refused.\n"
        "Configuration comes from the environment, optionally backed by a local .env file - see\n"
        ".env.example and docs/PRIVACY.md.\n",
        boreaz::kVersionString);
}

void list_all(Engine& engine) {
    AudioDevice audio(engine);
    std::printf("audio outputs:\n");
    const auto outputs = audio.ports();
    if (outputs.empty())
        std::printf("  (none)\n");
    for (const AudioDevice::Port& port : outputs)
        std::printf("  %2u  %s%s\n", port.index, port.name.c_str(),
                    port.is_default ? "  [default]" : "");

    std::printf("midi inputs:\n");
    const auto inputs = MidiInput::ports();
    if (inputs.empty())
        std::printf("  (none)\n");
    for (const MidiInput::Port& port : inputs)
        std::printf("  %2u  %s\n", port.index, port.name.c_str());
}

// A written part, scheduled on the engine's own clock rather than this thread's, so what comes
// out is frame accurate and cannot drift while the audio thread runs ahead of it.
//
// `chord` replaces the part with N notes held for the whole run: that is how "CPU < 3% of one
// core" gets quoted with a load attached instead of a bare number, and how the 16 note case a
// listener would call a wall of sound becomes something a stranger can reproduce.
void schedule_demo(Engine& engine, double sample_rate, double seconds, int chord) {
    if (chord > 0) {
        // Spread over the compass, because the top octave and A0 cost different things.
        for (int i = 0; i < chord; ++i) {
            const auto note = static_cast<std::uint8_t>(24 + i * 72 / (chord > 1 ? chord - 1 : 1));
            engine.push_event(Event::note_on(static_cast<std::int64_t>(0.1 * sample_rate), 0, note,
                                             100));
        }
        return;
    }

    static constexpr int kPart[] = {60, 64, 67, 72, 71, 67, 64, 62};
    constexpr double kBeat = 0.22;
    constexpr double kBar = kBeat * 4.0;

    // Four voices at a time, because "a 16 note chord does not break" is one of the things this
    // phase has to survive and a single line would not exercise it.
    for (double at = 0.15; at + kBeat < seconds; at += kBar) {
        const auto bar = static_cast<std::size_t>(at / kBar) % std::size(kPart);
        for (std::size_t voice = 0; voice < 4; ++voice) {
            const int pitch = kPart[(bar + voice) % std::size(kPart)] + (voice == 3 ? 12 : 0);
            const auto note = static_cast<std::uint8_t>(pitch);
            const auto start = static_cast<std::int64_t>((at + static_cast<double>(voice) * kBeat) * sample_rate);
            const auto stop = start + static_cast<std::int64_t>(kBeat * 0.9 * sample_rate);
            engine.push_event(Event::note_on(start, 0, note, 96));
            engine.push_event(Event::note_off(stop, 0, note));
        }
    }
}

void print_report(const AudioDevice& audio, const MidiInput& midi, const Engine& engine,
                  double seconds) {
    const double rate = audio.sample_rate();
    const double period_ms =
        rate > 0.0 ? 1000.0 * static_cast<double>(audio.callback_frames()) / rate : 0.0;

    std::printf("\nreport after %.1f s\n", seconds);
    std::printf("  callbacks          %llu\n", static_cast<unsigned long long>(audio.callbacks()));
    std::printf("  late callbacks     %llu  (a gap over %.1f periods)\n",
                static_cast<unsigned long long>(audio.late_callbacks()),
                static_cast<double>(boreaz::io::CallbackWatchdog::kLateMargin));
    std::printf("  worst gap          %.2f ms\n", static_cast<double>(audio.worst_gap_ns()) / 1e6);
    std::printf("  gaps vs period     %llu on time, %llu 1.5-2x, %llu 2-4x, %llu 4-8x, %llu over 8x\n",
                static_cast<unsigned long long>(
                    audio.callback_gaps(boreaz::io::CallbackWatchdog::kBucketOnTime)),
                static_cast<unsigned long long>(
                    audio.callback_gaps(boreaz::io::CallbackWatchdog::kBucketSlight)),
                static_cast<unsigned long long>(
                    audio.callback_gaps(boreaz::io::CallbackWatchdog::kBucketDouble)),
                static_cast<unsigned long long>(
                    audio.callback_gaps(boreaz::io::CallbackWatchdog::kBucketQuad)),
                static_cast<unsigned long long>(
                    audio.callback_gaps(boreaz::io::CallbackWatchdog::kBucketBeyond)));
    std::printf("  period delivered   %d frames at %.0f Hz = %.2f ms\n", audio.callback_frames(),
                rate, period_ms);
    std::printf("  voices sounding    %d\n", engine.active_voices());
    std::printf("  engine cpu         %.2f %% of one core, render thread only\n",
                audio.cpu_fraction() * 100.0);
    std::printf("  device restarts    %llu\n", static_cast<unsigned long long>(audio.restarts()));
    std::printf("  midi               %llu events, %llu dropped, %llu bad bytes\n",
                static_cast<unsigned long long>(midi.events()),
                static_cast<unsigned long long>(midi.dropped_messages()),
                static_cast<unsigned long long>(midi.bad_bytes()));
    std::printf("  midi unplug        %llu lost, %llu reopen attempts\n",
                static_cast<unsigned long long>(midi.losses()),
                static_cast<unsigned long long>(midi.reconnects()));
    std::printf("\n  A late callback is a gap we caused. A glitch the system mixer introduces below\n"
                "  us is invisible to these counters, so this is evidence and not proof: the ear\n"
                "  on --demo is the other half of the criterion.\n");
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    Engine engine;
    std::string error;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];

        // A value-taking option that runs off the end of the command line is a mistake worth
        // stopping for, not something to guess at.
        auto take = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                error = std::string(argument) + " needs a value";
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (argument == "--version" || argument == "-v") {
            print_version();
            return kOk;
        }
        if (argument == "--help" || argument == "-h") {
            print_usage();
            return kOk;
        }
        if (argument == "--list") {
            list_all(engine);
            return kOk;
        }
        if (argument == "--device") {
            if (!take(options.device))
                break;
            continue;
        }
        if (argument == "--midi") {
            if (!take(options.midi))
                break;
            continue;
        }
        if (argument == "--latency") {
            std::string value;
            if (!take(value))
                break;
            if (std::sscanf(value.c_str(), "%d", &options.period_frames) != 1 ||
                options.period_frames < 16 || options.period_frames > 8192) {
                error = "--latency wants 16 to 8192 frames, got '" + value + "'";
                break;
            }
            continue;
        }
        if (argument == "--seconds") {
            std::string value;
            if (!take(value))
                break;
            if (std::sscanf(value.c_str(), "%lf", &options.seconds) != 1 || options.seconds <= 0.0 ||
                options.seconds > 3600.0) {
                error = "--seconds wants a number of seconds, got '" + value + "'";
                break;
            }
            continue;
        }
        if (argument == "--chord") {
            std::string value;
            if (!take(value))
                break;
            if (std::sscanf(value.c_str(), "%d", &options.chord) != 1 || options.chord < 1 ||
                options.chord > 16) {
                error = "--chord wants 1 to 16 notes, got '" + value + "'";
                break;
            }
            continue;
        }
        if (argument == "--demo") {
            options.demo = true;
            continue;
        }
        if (argument == "--report") {
            options.report = true;
            continue;
        }

        error = "unknown option '" + std::string(argument) + "'";
        break;
    }

    if (!error.empty()) {
        std::fprintf(stderr, "boreaz: %s (try --help)\n", error.c_str());
        return kBadUsage;
    }

    // Audio first: without an output there is nothing to do, and the engine needs the device's
    // own sample rate before it can schedule anything.
    AudioDevice audio(engine);
    if (!audio.open(options.device, options.period_frames, error)) {
        std::fprintf(stderr, "boreaz: %s\n       try --list to see what this machine has\n",
                     error.c_str());
        return kAudioFailed;
    }
    if (!audio.start(error)) {
        std::fprintf(stderr, "boreaz: %s\n", error.c_str());
        return kAudioFailed;
    }
    std::printf("audio: %s at %.0f Hz, asking for a %d frame period\n", audio.name().c_str(),
                audio.sample_rate(), options.period_frames);

    MidiInput midi;
    if (options.demo) {
        schedule_demo(engine, audio.sample_rate(), options.seconds, options.chord);
        std::printf("demo: %s, %.1f seconds. Ctrl+C stops it hard.\n",
                    options.chord > 0 ? "a held chord" : "a written part", options.seconds);
    } else if (options.midi.empty()) {
        // An empty selector means "the first input, if this machine has one", which is the
        // friendliest thing to do when the player just plugs something in and runs us.
        if (midi.open("", engine, error))
            std::printf("midi: %s\n", midi.port_name().c_str());
        else
            std::printf("midi: none (%s) - play with the mouse later, or try --demo\n",
                        error.c_str());
    } else if (!midi.open(options.midi, engine, error)) {
        std::fprintf(stderr, "boreaz: %s\n       try --list to see what this machine has\n",
                     error.c_str());
        return kMidiFailed;
    } else {
        std::printf("midi: %s\n", midi.port_name().c_str());
    }
    if (!options.demo)
        std::printf("playing. press Enter to stop, or Ctrl+C to stop right now.\n");

    // One loop, one thread, doing the things that must not happen on the audio callback: watch
    // for an unplugged keyboard, watch for a device the system stopped, and decide when to exit.
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::duration<double>(options.demo ? options.seconds
                                                                              : 3600.0);
    std::uint64_t losses = 0;
    bool stopped = false;
    // Enumerating MIDI ports costs a winmm call per pass, so only do it when there is a port to
    // keep or reconnect. With no keyboard asked for, the loop must not touch winmm at all.
    const bool midi_to_watch = midi.is_open();
    while (!stopped) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // winmm does not report an unplug, so presence is polled: our port going away drops every
        // voice, and a port that comes back is opened again.
        if (midi_to_watch)
            midi.poll(MidiInput::ports(), engine);
        if (midi.losses() > losses) {
            losses = midi.losses();
            std::fprintf(stderr, "boreaz: '%s' disappeared, so every note was dropped\n",
                         midi.port_name().c_str());
        }
        audio.poll();

        if (std::chrono::steady_clock::now() >= deadline)
            stopped = true;
        if (!options.demo && !stopped && std::cin.rdbuf()->in_avail() > 0)
            stopped = true; // Enter ends it cleanly; Ctrl+C ends it now
    }
    const double ran_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if (options.report)
        print_report(audio, midi, engine, ran_seconds);

    midi.close();
    audio.close();
    return kOk;
}
