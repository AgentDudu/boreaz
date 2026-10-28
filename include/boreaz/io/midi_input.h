#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "boreaz/engine/engine.h"
#include "boreaz/io/midi_parser.h"

namespace boreaz::io {

// Windows MIDI In, through winmm.dll. miniaudio has no MIDI API at all (checked in 0.11.25 and
// on master), so this is the whole wrapper: enumerate, open one port, parse, and notice when the
// port goes away.
//
// Threading: the WinMM callback runs on a thread winmm owns. It parses and pushes into the
// engine's ring - no allocation, no lock, no printing. Enumerating, opening, closing and
// watching for an unplugged port happen on the application thread.
//
// Short messages need no input buffers: winmm delivers them as MOM_DATA on its own. Long
// (system-exclusive) buffers are not requested, so a patch dump is never received, which is
// what a synthesizer that cannot store patches wants.
class MidiInput {
  public:
    struct Port {
        std::uint32_t index = 0;
        std::string name;
    };

    static std::vector<Port> ports();

    // `which` selects a port: a decimal index ("2"), a case insensitive substring of a port
    // name, or empty for the first one. "no ports" is not an error - a synthesizer with no
    // keyboard attached still runs, and the on-screen keyboard arrives with the GUI.
    bool open(const std::string& which, engine::Engine& engine, std::string& error);
    void close();

    bool is_open() const { return open_.load(std::memory_order_relaxed); }
    const std::string& port_name() const { return name_; }

    // Point the parser at an engine without opening a port, for use before a port exists (a
    // keyboard that is still unplugged) and in tests: this is the only way the push path can be
    // exercised on a machine with no MIDI input at all. Call it while closed - a running
    // callback is reading the same pointer.
    void attach(engine::Engine& engine) { engine_ = &engine; }

    // Application thread. winmm does not report an unplug reliably, so the caller passes the
    // port list it has just enumerated: when ours has gone, every voice is dropped; when the
    // port we asked for comes back, it is opened again.
    void poll(const std::vector<Port>& ports, engine::Engine& engine);

    // Port lookup, separate from winmm so the unplug decisions are testable on a machine with
    // no keyboard.
    static const Port* find(const std::vector<Port>& ports, const std::string& which);
    static bool present(const std::vector<Port>& ports, const std::string& name);

    // Stop everything the port was holding and mark it gone. Public because that is the
    // half of the unplug path a test can reach without a driver: hold some notes, call this,
    // and the engine must be silent with no voice left behind.
    void handle_loss(engine::Engine& engine);

    std::uint64_t events() const { return events_.load(std::memory_order_relaxed); }
    std::uint64_t bad_bytes() const { return bad_bytes_.load(std::memory_order_relaxed); }
    // Messages a driver delivered that did not start with a status byte.
    std::uint64_t dropped_messages() const { return dropped_messages_.load(std::memory_order_relaxed); }
    std::uint64_t sysex_bytes() const { return sysex_bytes_.load(std::memory_order_relaxed); }
    std::uint64_t losses() const { return losses_.load(std::memory_order_relaxed); }
    // Reopen attempts, successful or not: a port that comes back with a driver problem should
    // still show up here.
    std::uint64_t reconnects() const { return reconnects_.load(std::memory_order_relaxed); }

    // One packed MOM_DATA message from the winmm callback thread. Public because that thread is
    // a static function; unit tests drive the same door, which is how the parser is tested
    // against what a driver actually delivers.
    void on_message(std::uint32_t packed);

  private:
    bool open_port(const Port& port, engine::Engine& engine, std::string& error);

    // winmm's handle, kept as a pointer so this header never includes windows.h.
    void* handle_ = nullptr;
    std::string name_;   // the port currently (or last) open
    std::string wanted_; // what the caller asked for, reused to reopen after an unplug
    engine::Engine* engine_ = nullptr;
    MidiParser parser_; // winmm callback thread only

    std::atomic<bool> open_{false};
    std::atomic<bool> driver_closed_{false};
    std::atomic<std::uint64_t> events_{0};
    std::atomic<std::uint64_t> bad_bytes_{0};
    std::atomic<std::uint64_t> dropped_messages_{0};
    std::atomic<std::uint64_t> sysex_bytes_{0};
    std::atomic<std::uint64_t> losses_{0};
    std::atomic<std::uint64_t> reconnects_{0};
};

} // namespace boreaz::io
