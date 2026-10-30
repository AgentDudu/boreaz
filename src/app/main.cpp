// The instrument's front door. It opens a device, opens a port, opens a window, and stops when
// the player says so; the policy of what a note means lives in the engine, and the geometry of a
// key lives in the layout. What is here is argument parsing and a run loop, which is the smallest
// honest description of an application shell.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "boreaz/config/params_file.h"
#include "boreaz/engine/engine.h"
#include "boreaz/gui/app_window.h"
#include "boreaz/io/audio_device.h"
#include "boreaz/io/midi_input.h"
#include "boreaz/midifile/midifile.h"

namespace {

std::atomic<bool> stopping{false};

void asked_to_stop(int signal_number) {
    (void)signal_number;
    stopping.store(true);
}

void usage() {
    std::cout << R"(boreaz - a polyphonic subtractive software synthesizer

  boreaz [--gui] [--device NAME] [--period 512] [--midi NAME] [--config params.ini]
             [--params other.ini] [song.mid] [--list-devices] [--list-midi] [--help]

  --gui            open the window: 88 playable keys and a readout of what the audio
                   thread is doing. Without it the app runs headless until Ctrl+C,
                   which is what a machine with no display, or a CI runner, can do.
  --device NAME    output to open: empty is the system default, or an index, or part
                   of a port name. See --list-devices.
  --midi NAME      a MIDI keyboard to listen to (index, name, or "auto" for the first
                   port). "none" declines to open one. A keyboard that is unplugged
                   later is noticed and its notes are dropped.
  --config FILE    parameter defaults, instead of config/params.ini. An unreadable file
                   or an unknown key is an error and the app refuses to start, because
                   silent defaults are indistinguishable from working ones.
  --params FILE    a second file applied on top of --config, for a saved patch.

  song.mid         a Standard MIDI file, played once through: in the window with --gui,
                   in the console without it, where the program exits when the file has
                   finished. Name it last. Format 0 or 1 with ticks per quarter note; a
                   file the reader does not understand is refused with a message rather
                   than played wrong. boreaz-render renders the same files to a WAV.)"
           << std::endl;
}

struct Options {
    std::string config; // empty means the instrument's own defaults, wherever they are

    std::string params;
    std::string device;
    std::string midi = "auto";
    int period = 512;
    bool gui = false;
    bool list_devices = false;
    bool list_midi = false;
    // A file to play once through. Empty, which is how the program has always started, means no
    // file: a blank instrument waiting for a keyboard.
    std::string song;
};

bool parse(int argc, char** argv, Options& options, std::string& error) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next = [&](std::string& into) {
            if (index + 1 >= argc) {
                error = argument + " needs a value";
                return false;
            }
            into = argv[++index];
            return true;
        };
        if (argument == "--") {
            // "boreaz -- song.mid" reads the way people type it. A bare path works too, so this is
            // a separator that means nothing else.
            continue;
        } else if (argument == "--help" || argument == "-h") {
            usage();
            std::exit(0);
        } else if (argument == "--gui") {
            options.gui = true;
        } else if (argument == "--list-devices") {
            options.list_devices = true;
        } else if (argument == "--list-midi") {
            options.list_midi = true;
        } else if (argument == "--device") {
            if (!next(options.device))
                return false;
        } else if (argument == "--midi") {
            if (!next(options.midi))
                return false;
        } else if (argument == "--config") {
            if (!next(options.config))
                return false;
        } else if (argument == "--params") {
            if (!next(options.params))
                return false;
        } else if (argument == "--period") {
            std::string value;
            if (!next(value))
                return false;
            options.period = std::atoi(value.c_str());
            // A driver rounds a period to its own granularity, so asking for 1 is refused here
            // rather than quietly becoming 512 and disappointing someone who meant it.
            if (options.period < 16 || options.period > 8192) {
                error = "--period must be between 16 and 8192 frames";
                return false;
            }
        } else if (!argument.empty() && argument[0] != '-') {
            if (!options.song.empty()) {
                error = "one file at a time, not two (try --help)";
                return false;
            }
            options.song = argument;
        } else {
            error = "unknown argument: " + argument + " (try --help)";
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    std::string error;
    if (!parse(argc, argv, options, error)) {
        std::cerr << "boreaz: " << error << "\n";
        return 1;
    }

    // Defaults come from a file, and a file that will not load stops the app. An instrument that
    // starts with silent defaults because a config file was missing teaches the player that their
    // new synth is broken.
    boreaz::engine::Params params;
    // --config replaces the shipped defaults; --params then layers a patch on top of whichever one
    // was used, which is the only order that lets a patch name just the keys it changes.
    const std::string defaults =
        options.config.empty() ? boreaz::config::defaults_path() : options.config;
    if (!boreaz::config::load_params_file(defaults, params, error) ||
        (!options.params.empty() &&
         !boreaz::config::load_params_file(options.params, params, error))) {
        std::cerr << "boreaz: " << error << "\n";
        return 1;
    }

    boreaz::engine::Engine engine(params);
    boreaz::io::AudioDevice device(engine);
    boreaz::io::MidiInput midi;

    if (options.list_devices) {
        const std::size_t width = std::max<std::size_t>(30u, options.device.size());
        for (const boreaz::io::AudioDevice::Port& port : device.ports()) {
            std::cout << port.index << ". " << port.name;
            if (port.is_default)
                std::cout << std::string(width - std::min(width, port.name.size()), ' ') << "[default]";
            std::cout << "\n";
        }
        return 0;
    }
    if (options.list_midi) {
        const auto ports = boreaz::io::MidiInput::ports();
        if (ports.empty())
            std::cout << "no MIDI input ports. A keyboard that appears later is picked up by --midi auto.\n";
        for (const boreaz::io::MidiInput::Port& port : ports)
            std::cout << port.index << ". " << port.name << "\n";
        return 0;
    }

    if (!device.open(options.device, options.period, error)) {
        std::cerr << "boreaz: " << error << "\n";
        return 1;
    }
    if (options.midi != "none" && !midi.open(options.midi, engine, error)) {
        // Not fatal. A synthesizer whose one MIDI keyboard is unplugged plays from the mouse, and
        // a program that refused to start would be less useful than one that says so.
        std::cerr << "boreaz: MIDI in: " << error << "\n";
    }
    // A file to play. Read after the device opens, because the sample rate is what turns its ticks
    // into samples, and before it starts, because a file that cannot be parsed should stop the app
    // rather than sit in silence looking like it was trying.
    boreaz::midifile::Song song;
    bool playing = false;
    std::size_t song_cursor = 0;
    std::size_t song_dropped = 0;
    if (!options.song.empty()) {
        boreaz::midifile::LoadResult loaded =
            boreaz::midifile::load_file(options.song, device.sample_rate());
        if (!loaded.ok) {
            std::cerr << "boreaz: " << loaded.error << "\n";
            midi.close();
            device.close();
            return 1;
        }
        for (const std::string& warning : loaded.warnings)
            std::cerr << "boreaz: " << options.song << ": " << warning << "\n";
        song = std::move(loaded.song);
        playing = true;
        std::cout << "boreaz: \"" << options.song << "\": " << song.events.size() << " events, "
                  << static_cast<double>(song.end_sample) / device.sample_rate() << " seconds."
                  << std::endl;
    }

    if (!device.start(error)) {
        std::cerr << "boreaz: " << error << "\n";
        device.close();
        return 1;
    }

    std::signal(SIGINT, &asked_to_stop);
#ifdef SIGTERM
    std::signal(SIGTERM, &asked_to_stop);
#endif

    // The period the callback is really getting is a number the audio thread only knows once it has
    // run, and the panel shows it from the first frame. Before any callback has happened this line
    // can honestly say only what the player asked for, so it says that instead of a zero that
    // reads like a broken device.
    std::cout << "boreaz: " << device.name() << ", " << static_cast<int>(device.sample_rate())
              << " Hz, " << options.period << " frame period, "
              << (options.gui ? "window" : "headless") << ". Ctrl+C stops." << std::endl;

    const std::string port_name = device.name();
    auto stats = [&](boreaz::gui::DeviceStats& out, boreaz::gui::MidiStats& in) {
        out.open = device.is_open();
        out.name = port_name;
        out.sample_rate = device.sample_rate();
        out.callback_frames = device.callback_frames();
        out.callbacks = device.callbacks();
        out.late_callbacks = device.late_callbacks();
        out.restarts = device.restarts();
        out.cpu_fraction = device.cpu_fraction();
        in.open = midi.is_open();
        in.name = midi.port_name();
        in.losses = midi.losses();
        in.events = midi.events();
    };

    std::unique_ptr<boreaz::gui::AppWindow> window;
    if (options.gui) {
        window = std::make_unique<boreaz::gui::AppWindow>();
        if (!window->open({}, error)) {
            // The window is the app, but not being able to draw is not a reason to stop a device
            // that is already making sound: say which one failed and carry on without it.
            std::cerr << "boreaz: no window: " << error << "\n  continuing without one; Ctrl+C stops.\n";
            window.reset();
        }
    }
    if (window != nullptr && playing)
        window->set_external_playback(true);

    // Events are queued a little ahead of the audio thread, because the engine applies an event on
    // the sample it names: early costs nothing and late moves a note. Half a second covers this
    // loop's own sleep, a window frame, and a driver that stalls a callback now and then, while
    // staying far below the 4096 slots the engine's event ring holds.
    const std::int64_t lookahead = static_cast<std::int64_t>(device.sample_rate() * 0.5);

    // Unplug detection is polled, because winmm does not report it. Once every quarter second,
    // not once per frame: enumeration is a driver call per port, and 60 of those a second buys
    // nothing a player would notice.
    double last_poll = 0.0;
    while (true) {
        if (playing) {
            const boreaz::midifile::FeedResult fed = boreaz::midifile::feed_up_to(
                engine, song, song_cursor, engine.sample_position() + lookahead);
            song_cursor = fed.cursor;
            song_dropped += fed.dropped;
            if (song_cursor >= song.events.size() &&
                engine.sample_position() >= song.end_sample) {
                // The last event is in and its tail has rung out. In a window a file is one song
                // among many, so park the clock on its length and stay open; with no window the
                // file was the whole session, so stop.
                playing = false;
                if (window) {
                    window->set_external_playback(false);
                    window->park_transport();
                }
                std::cout << "boreaz: \"" << options.song << "\" finished"
                          << (song_dropped == 0u
                                  ? ""
                                  : " - " + std::to_string(song_dropped) + " events dropped")
                          << "." << std::endl;
                if (window == nullptr)
                    break;
            }
        }
        const bool running = window ? window->frame(engine, stats) : !stopping.load();
        if (!running || stopping.load())
            break;
        const double now = window ? window->seconds_elapsed()
                                  : static_cast<double>(
                                        std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now().time_since_epoch())
                                            .count()) /
                                        1000.0;
        if (now - last_poll < 0.25) {
            if (window == nullptr)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        last_poll = now;
        device.poll();
        midi.poll(boreaz::io::MidiInput::ports(), engine);
    }

    // Numbers, not adjectives. Everything on this line came from a counter the engine or the
    // device keeps, and this is the line a claim about glitches is argued from.
    std::cout << "boreaz: " << device.callbacks() << " callbacks, " << device.late_callbacks()
              << " late, " << device.restarts() << " restarts, " << engine.dropped_events()
              << " dropped, " << engine.steal_count()
              << " steals, position " << engine.sample_position() << " samples, "
              << (window ? window->frames() : 0.0) << " frames." << std::endl;
    device.stop();
    midi.close();
    device.close();
    return 0;
}
