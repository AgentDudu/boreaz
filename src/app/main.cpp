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

#include "boreaz/config/app_settings.h"
#include "boreaz/config/params_file.h"
#include "boreaz/engine/engine.h"
#include "boreaz/gui/app_window.h"
#include "boreaz/io/audio_device.h"
#include "boreaz/io/midi_input.h"
#include "boreaz/midifile/midifile.h"
#include "boreaz/samples/sample_store.h"

namespace {

std::atomic<bool> stopping{false};

void asked_to_stop(int signal_number) {
    (void)signal_number;
    stopping.store(true);
}

void usage() {
    std::cout << R"(boreaz - a polyphonic subtractive software synthesizer

  boreaz [--gui] [--device NAME] [--period 512] [--midi NAME] [--config params.ini]
             [--params other.ini] [--instrument piano.sfz] [song.mid] [--list-devices]
             [--list-midi] [--help]

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
  --gain DB        master output level in dB, -72 to +12, overriding whatever a file said. The
                   engine starts at -12 dB on purpose - sixteen voices summed need room - so this
                   is the flag for "it is too quiet" and "it is clipping".
  --instrument FILE  play a recorded instrument instead of the synthesiser: an SFZ mapping,
                   with its WAVs next to it. Keys the library did not record play the nearest
                   recording it has, shifted, which is what a piano with 37 samples for 88 keys
                   sounds like. If it cannot be read the app says so and plays the synthesiser,
                   because an instrument that will not load must not mean silence. Build one
                   with tools/mkinstrument.py; see boreaz-sfz-probe for what a mapping says.
  --no-instrument  the synthesiser, whatever else might have been configured.

  The app remembers its own setup: the audio port, the MIDI port, the sample library, and the patch
  that was open when the window last closed. That is four names in %APPDATA%\Boreaz\settings.txt,
  read at the next start and written at the end of a window session. A flag typed at the prompt
  outranks it, because what was typed is the newest thing said about the setup, and a remembered
  port that is not there any more falls back to the system default rather than refusing to start.
  It holds no sound: parameters live in patches, and closing the app never rewrites one.

  song.mid         a Standard MIDI file, played once through: in the window with --gui,
                   in the console without it, where the program exits when the file has
                   finished. Name it last. Format 0 or 1 with ticks per quarter note; a
                   file the reader does not understand is refused with a message rather
                   than played wrong. boreaz-render renders the same files to a WAV.
)"
              << "\n  parameters a --config or --params file sets" << std::endl
              << "  " << boreaz::config::known_keys() << "\n"
              << "  every one of them is also a slider in the window's Mixer." << std::endl;
}

struct Options {
    std::string config; // empty means the instrument's own defaults, wherever they are

    std::string params;
    // "--gain" is a flag because "it is too quiet" is a thing a player finds out in the first ten
    // seconds, and a parameter file is not what you reach for then. Kept as text: the parameter
    // table owns the range and the message, so this cannot drift from the slider.
    std::string gain;
    std::string device;
    std::string midi = "auto";
    // Whether the player said it. An empty `device` and an `auto` `midi` are also what the program
    // already does, so a remembered setup cannot tell "the system default" from "leave the one I
    // wrote in the setup file alone" without being told which was spoken at the prompt.
    bool midi_given = false;
    bool instrument_given = false;
    int period = 512;
    bool gui = false;
    bool list_devices = false;
    bool list_midi = false;
    // A file to play once through. Empty, which is how the program has always started, means no
    // file: a blank instrument waiting for a keyboard.
    std::string song;
    // A sample library to play instead of the synthesiser. Empty keeps the oscillators, which is
    // still the default instrument and the only one that needs nothing downloaded.
    std::string instrument;
    bool no_instrument = false;
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
            options.midi_given = true;
        } else if (argument == "--config") {
            if (!next(options.config))
                return false;
        } else if (argument == "--params") {
            if (!next(options.params))
                return false;
        } else if (argument == "--gain") {
            if (!next(options.gain))
                return false;
        } else if (argument == "--instrument") {
            if (!next(options.instrument))
                return false;
            options.instrument_given = true;
        } else if (argument == "--no-instrument") {
            options.no_instrument = true;
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

    // What this app was closed with last time: an audio port, a MIDI port, a sample library, a
    // patch. It is read before anything is opened, because "come back the way you left me" is a
    // statement about what to open, not about what things sound like - and it is read line by line,
    // so a setup file written by a newer build of the app starts anyway and says which word it did
    // not know. The parameter file runs the opposite rule, on purpose: a half-read sound is a sound
    // nobody wrote.
    boreaz::config::AppSettings setup;
    std::string setup_warning;
    boreaz::config::load_settings(boreaz::config::settings_path(), setup, setup_warning);
    if (!setup_warning.empty())
        std::cout << "boreaz: " << setup_warning << "\n";

    // Three sources of the same answer, newest first: what was typed at the prompt, what this app
    // wrote when it was last closed, and what the program does when nobody said anything. A
    // remembered library is not applied when --no-instrument was typed, because that flag is the
    // player saying they do not want one - today's words outrank last month's.
    const bool device_from_setup = options.device.empty() && !setup.audio_device.empty();
    if (device_from_setup)
        options.device = setup.audio_device;
    if (!options.midi_given && !setup.midi_port.empty())
        options.midi = setup.midi_port;
    if (!options.instrument_given && !options.no_instrument && !setup.instrument.empty())
        options.instrument = setup.instrument;

    // Defaults come from a file, and a file that will not load stops the app. An instrument that
    // starts with silent defaults because a config file was missing teaches the player that their
    // new synth is broken.
    boreaz::engine::Params params;
    // --config replaces the shipped defaults; --params then layers a patch on top of whichever one
    // was used, which is the only order that lets a patch name just the keys it changes.
    const std::string defaults =
        options.config.empty() ? boreaz::config::defaults_path() : options.config;
    if (!boreaz::config::load_params_file(defaults, params, error)) {
        std::cerr << "boreaz: " << error << "\n";
        return 1;
    }

    // The patch that was open when the app last closed, unless today's prompt named one. A path the
    // app wrote itself is not the same kind of promise as a path someone typed: the file may have
    // been deleted or hand-edited since, and an instrument that will not start over its own memory
    // is an instrument with no way out. So this one reports and plays without it.
    std::string remembered_patch = options.params;
    if (remembered_patch.empty() && !setup.patch.empty()) {
        std::string reason;
        if (boreaz::config::load_params_file(setup.patch, params, reason)) {
            remembered_patch = setup.patch;
        } else {
            std::cout << "boreaz: starting without the patch from last time: " << reason << "\n";
        }
    }

    // --params layers on top of whichever file was used, and after the remembered patch: what the
    // player typed today is the newest thing said about the sound.
    if (!options.params.empty() &&
        !boreaz::config::load_params_file(options.params, params, error)) {
        std::cerr << "boreaz: " << error << "\n";
        return 1;
    }
    // Last, so the flag wins over both files: what was typed at the prompt is the newest thing said
    // about the level.
    if (!options.gain.empty() && !boreaz::config::set_param_from_text(
                                     "--gain", "master_gain_db", options.gain, params, error)) {
        std::cerr << "boreaz: " << error << "\n";
        return 1;
    }

    if (options.no_instrument && !options.instrument.empty()) {
        std::cerr << "boreaz: --no-instrument and --instrument: say one or the other\n";
        return 1;
    }

    // A sample library is declared before the engine and loaded before the device opens: a voice
    // keeps a pointer into the instrument for as long as it is sounding, so the store has to
    // outlive the engine, and adopting an instrument rebuilds the voice pool, which is not a thing
    // to do underneath a running audio thread. Everything about the library is decided here, at
    // load, because the audio thread never opens a file (PLAN.md 2).
    //
    // Two stores because the mixer can ask for another library while this one is playing. Loading
    // into the live store would be file I/O and a clear() underneath the audio thread; the mixer
    // fills the idle one and switches to it once the notes have stopped, which is also the first
    // moment the store it replaces is unreferenced.
    std::array<std::unique_ptr<boreaz::samples::SampleStore>, 2> stores;
    stores[0] = std::make_unique<boreaz::samples::SampleStore>();
    stores[1] = std::make_unique<boreaz::samples::SampleStore>();
    const boreaz::engine::Instrument* live_instrument = nullptr;
    // Which library is actually playing, in the words a library field takes. Empty is the
    // synthesiser. The window's Load button and a patch's header are the only things that change it
    // after the start, so the app has to be told when they do - it is what the setup file gets.
    std::string live_library;
    boreaz::engine::Engine engine(params);
    if (!options.no_instrument && !options.instrument.empty()) {
        const boreaz::samples::LoadResult loaded = stores[0]->load_mapping(options.instrument);
        if (!loaded.ok) {
            // Loud, and then carry on. PLAN.md phase 9: a missing or broken instrument falls back
            // to the synth with a visible warning, never silence. A player who typed the path can
            // read this line; a player who double clicked the icon hears a piano of some kind and
            // a console that says which one they got.
            std::cerr << "boreaz: " << loaded.error << "\n"
                      << "boreaz: no recorded instrument, playing the synthesiser instead\n";
        } else {
            live_instrument = stores[0].get();
            live_library = options.instrument;
            engine.set_instrument(live_instrument);
            // What it loaded, what it cost, and what it refused: a library that arrived as 40 of
            // its 88 keys is not the library the player downloaded, and silence does not say so.
            std::cout << "samples: " << loaded.regions << " regions";
            if (loaded.releases != 0)
                std::cout << " and " << loaded.releases << " release recordings";
            std::cout << " from " << loaded.files << " files, " << loaded.bytes / (1024u * 1024u)
                      << " MB resident, " << loaded.load_ms << " ms to load";
            if (loaded.refused != 0)
                std::cout << ", " << loaded.refused << " regions refused";
            std::cout << "\n";
            for (const std::string& warning : loaded.warnings)
                std::cout << "  " << warning << "\n";
        }
    }
    boreaz::io::AudioDevice device(engine);
    boreaz::io::MidiInput midi;

    if (options.list_devices) {
        const std::size_t width = std::max<std::size_t>(30u, options.device.size());
        for (const boreaz::io::AudioDevice::Port& port : device.ports()) {
            std::cout << port.index << ". " << port.name;
            if (port.is_default)
                std::cout << std::string(width - std::min(width, port.name.size()), ' ')
                          << "[default]";
            std::cout << "\n";
        }
        return 0;
    }
    if (options.list_midi) {
        const auto ports = boreaz::io::MidiInput::ports();
        if (ports.empty())
            std::cout << "no MIDI input ports. A keyboard that appears later is picked up by "
                         "--midi auto.\n";
        for (const boreaz::io::MidiInput::Port& port : ports)
            std::cout << port.index << ". " << port.name << "\n";
        return 0;
    }

    if (!device.open(options.device, options.period, error)) {
        if (!device_from_setup) {
            std::cerr << "boreaz: " << error << "\n";
            return 1;
        }
        // A port this app remembered is a memory, not an instruction. The interface was asleep, the
        // cable was pulled, the laptop is somewhere else without its dock: the old device went away
        // and the player typed nothing today. Say what was lost, then take the system default,
        // which is the sound of the instrument starting anyway. A name typed at the prompt is
        // different - that one fails, because a person asked for that port and deserves to be told
        // they did not get it.
        std::cerr << "boreaz: " << error << "\n"
                  << "boreaz: that is the port this app last used, not one you asked for; "
                     "opening the system default instead\n";
        error.clear();
        if (!device.open(std::string{}, options.period, error)) {
            std::cerr << "boreaz: " << error << "\n";
            return 1;
        }
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
        // The mixer asks and this loads, because a library is opened with file I/O and the audio
        // thread never touches a file. The store returned is the one that was not playing; the
        // window adopts it when the pool goes quiet and tells us here, which is when the store
        // before it stops being referenced and can be dropped.
        boreaz::gui::AppWindow::Options window_options;
        window_options.instruments.load =
            [&](const std::string& path, std::string& reason) -> const boreaz::engine::Instrument* {
            if (path.empty()) {
                reason.clear();
                live_library
                    .clear(); // the synthesiser, which is not an error and is worth remembering
                return nullptr;
            }
            std::unique_ptr<boreaz::samples::SampleStore>& idle =
                live_instrument == stores[0].get() ? stores[1] : stores[0];
            const boreaz::samples::LoadResult loaded = idle->load_mapping(path);
            if (!loaded.ok) {
                reason = loaded.error;
                return nullptr;
            }
            if (loaded.regions == 0) {
                reason = "no samples in \"" + path + "\"";
                return nullptr;
            }
            std::cout << "samples: " << loaded.regions << " regions";
            if (loaded.releases != 0)
                std::cout << " and " << loaded.releases << " release recordings";
            std::cout << ", " << loaded.bytes / (1024u * 1024u) << " MB, " << loaded.load_ms
                      << " ms to load - switching when the notes stop.\n";
            // What the mixer asked for and got, recorded here rather than in `applied`: a library
            // still waiting for the notes to stop is still the library the player chose, and a
            // session that ends mid-switch should remember the choice rather than the one before
            // it.
            live_library = path;
            return idle.get();
        };
        window_options.instruments.applied = [&](const boreaz::engine::Instrument* instrument) {
            if (live_instrument == instrument)
                return;
            // The panel only fires this once the voice pool is empty, but an empty pool is still a
            // pool the callback may be rendering: adopting an instrument rebuilds it, and a rebuild
            // that lands inside a render is a voice vanishing mid-sample. Stopping the device is
            // the one moment that is known to be between callbacks, and the panel already promised
            // this is not silent - the gap is a few milliseconds of buffer, once, at a switch.
            device.stop();
            engine.set_instrument(instrument);
            if (playing) {
                std::string restart;
                if (!device.start(restart))
                    std::cerr << "boreaz: " << restart << " - sound stopped after the switch\n";
            }
            if (live_instrument != nullptr) {
                for (std::unique_ptr<boreaz::samples::SampleStore>& store : stores)
                    if (store != nullptr && store.get() == live_instrument)
                        store = std::make_unique<boreaz::samples::SampleStore>();
            }
            live_instrument = instrument;
        };
        if (!window->open(window_options, error)) {
            // The window is the app, but not being able to draw is not a reason to stop a device
            // that is already making sound: say which one failed and carry on without it.
            std::cerr << "boreaz: no window: " << error
                      << "\n  continuing without one; Ctrl+C stops.\n";
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
            if (song_cursor >= song.events.size() && engine.sample_position() >= song.end_sample) {
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
        const double now =
            window ? window->seconds_elapsed()
                   : static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
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
    // The cpu figure is the audio thread's own busy time over the wall time the device was running
    // - the number PLAN's "< 8% of one core" is about, and the only way to read it off a run
    // without a profiler.
    std::cout << "boreaz: " << device.callbacks() << " callbacks, " << device.late_callbacks()
              << " late, " << device.restarts() << " restarts, " << engine.dropped_events()
              << " dropped, " << engine.steal_count() << " steals, position "
              << engine.sample_position() << " samples, " << (window ? window->frames() : 0.0)
              << " frames, " << (device.cpu_fraction() * 100.0) << "% audio-thread cpu."
              << std::endl;
    device.stop();

    // Ending a session at the mixer is when the app writes down what it was using, so the next one
    // opens the same way. A run that never had a window does not write: playing one file from the
    // prompt is a command, not a preference, and remembering the port a test happened to use would
    // change what the player hears when they double click the icon.
    //
    // Four names, and no parameters. The sound lives in patch files a player can read and edit, and
    // an app that rewrote one on the way out would be changing a file nobody asked it to change -
    // so this remembers which patch was open, and the patch keeps its own bytes.
    if (window != nullptr) {
        boreaz::config::AppSettings closing;
        closing.audio_device = port_name; // the port that answered, not the word that asked for it
        closing.midi_port = midi.is_open() ? midi.port_name() : options.midi;
        // The library that resolved, by the name it resolved under - absolute. Asked for as a path
        // relative to where this session happened to start, remembered where the next one will find
        // it wherever it starts. A patch file keeps the name its author typed: that one is written
        // to travel between machines, and this one is read by a single machine about itself.
        closing.instrument = boreaz::config::resolved_path(live_library);
        // What the mixer clicked since it opened, or what it was opened with. The panel has no way
        // to un-adopt a patch, so an empty answer here means nothing was clicked and the start
        // stands. Absolute for the same reason as the library: the patch is found by a folder the
        // app locates by walking up the tree, and the memory is read back by a start that may begin
        // anywhere - including a build tree, which is where the suite runs from.
        const std::string open_patch =
            window->last_patch().empty() ? remembered_patch : window->last_patch();
        closing.patch = boreaz::config::resolved_path(open_patch);
        std::string reason;
        if (!boreaz::config::save_settings(boreaz::config::settings_path(), closing, reason))
            std::cerr << "boreaz: " << reason << " - the next start will not remember this one\n";
    }

    midi.close();
    device.close();
    return 0;
}
