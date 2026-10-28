#include "boreaz/io/midi_input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mmsystem.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

namespace boreaz::io {
namespace {

std::string utf8(const WCHAR* wide) {
    if (wide == nullptr || wide[0] == 0)
        return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1)
        return {};
    std::string out(static_cast<std::size_t>(need) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), need, nullptr, nullptr);
    return out;
}

std::string describe(MMRESULT result) {
    char text[128] = {0};
    if (midiInGetErrorTextA(result, text, static_cast<UINT>(sizeof text)) != MMSYSERR_NOERROR)
        std::snprintf(text, sizeof text, "winmm error %u", static_cast<unsigned>(result));
    return text;
}

bool contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty())
        return true;
    const auto ci = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [&](char a, char b) { return ci(a) == ci(b); }) != haystack.end();
}

// winmm's own thread. Only MIM_DATA is acted on: short messages need no buffer, long (sysex)
// buffers are never requested, and winmm has no reliable unplug notification, which is why
// MidiInput::poll exists.
void CALLBACK on_winmm_message(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR data, DWORD) {
    auto* self = reinterpret_cast<MidiInput*>(instance);
    if (self == nullptr)
        return;
    if (message == MIM_DATA || message == MIM_MOREDATA)
        self->on_message(static_cast<std::uint32_t>(data));
}

} // namespace

std::vector<MidiInput::Port> MidiInput::ports() {
    std::vector<Port> out;
    const UINT count = midiInGetNumDevs();
    out.reserve(count);
    MIDIINCAPSW caps{};
    for (UINT i = 0; i < count; ++i) {
        if (midiInGetDevCapsW(i, &caps, static_cast<UINT>(sizeof caps)) != MMSYSERR_NOERROR)
            continue;
        out.push_back(Port{i, utf8(caps.szPname)});
    }
    return out;
}

const MidiInput::Port* MidiInput::find(const std::vector<Port>& ports, const std::string& which) {
    if (ports.empty())
        return nullptr;
    if (which.empty())
        return &ports.front();
    if (std::all_of(which.begin(), which.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        const auto index = static_cast<std::uint32_t>(std::stoul(which));
        for (const Port& port : ports)
            if (port.index == index)
                return &port;
        return nullptr;
    }
    for (const Port& port : ports)
        if (contains(port.name, which))
            return &port;
    return nullptr;
}

bool MidiInput::present(const std::vector<Port>& ports, const std::string& name) {
    return std::any_of(ports.begin(), ports.end(),
                       [&](const Port& port) { return port.name == name; });
}

bool MidiInput::open_port(const Port& port, engine::Engine& engine, std::string& error) {
    // The callback may fire the instant the port opens, so everything it reads is set first.
    engine_ = &engine;
    name_ = port.name;

    HMIDIIN handle = nullptr;
    MMRESULT result = midiInOpen(&handle, port.index,
                                 reinterpret_cast<DWORD_PTR>(&on_winmm_message),
                                 reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        engine_ = nullptr;
        name_.clear();
        error = "midiInOpen port " + std::to_string(port.index) + ": " + describe(result);
        return false;
    }
    result = midiInStart(handle);
    if (result != MMSYSERR_NOERROR) {
        midiInClose(handle);
        engine_ = nullptr;
        name_.clear();
        error = "midiInStart: " + describe(result);
        return false;
    }

    handle_ = handle;
    open_.store(true, std::memory_order_relaxed);
    return true;
}

bool MidiInput::open(const std::string& which, engine::Engine& engine, std::string& error) {
    wanted_ = which;
    const std::vector<Port> available = ports();
    const Port* port = find(available, which);
    if (port == nullptr) {
        error = which.empty() ? std::string("no MIDI input port found")
                              : "no MIDI input port matches '" + which + "'";
        return false;
    }
    if (!open_port(*port, engine, error))
        return false;
    // "The first one" is not something poll() can look for again, so remember what it resolved
    // to: reconnecting then finds the same keyboard by name.
    if (wanted_.empty())
        wanted_ = port->name;
    return true;
}

void MidiInput::close() {
    open_.store(false, std::memory_order_relaxed);
    if (handle_ == nullptr)
        return;
    const auto handle = static_cast<HMIDIIN>(handle_);
    midiInStop(handle);
    // midiInClose returns once the driver is done calling back, which is what makes it safe to
    // drop engine_ on the next line.
    midiInClose(handle);
    handle_ = nullptr;
    engine_ = nullptr;
}

void MidiInput::handle_loss(engine::Engine& engine) {
    close();
    // A port that disappears mid-phrase would otherwise leave every held note sounding, and a
    // pedal nobody is pressing. Both go through the queue rather than a direct call because the
    // queue is the only way into the audio thread, and it is where the engine's own panic lives.
    const std::int64_t tick = engine.sample_position();
    engine.push_event(engine::Event::all_notes_off(tick, 0));
    if (engine.sustain_pedal_down())
        engine.push_event(engine::Event::control(tick, 0, engine::Event::kSustainPedal, 0));
    losses_.fetch_add(1, std::memory_order_relaxed);
}

void MidiInput::poll(const std::vector<Port>& available, engine::Engine& engine) {
    if (is_open() && !present(available, name_))
        handle_loss(engine);

    if (is_open() || wanted_.empty())
        return;
    const Port* port = find(available, wanted_);
    if (port == nullptr)
        return;
    // Repeated attempts are counted even when they fail, so a port that is back but sick is
    // visible rather than silent.
    reconnects_.fetch_add(1, std::memory_order_relaxed);
    std::string error;
    open_port(*port, engine, error);
}

void MidiInput::on_message(std::uint32_t packed) {
    // A driver can deliver a message whose first byte is not a status byte. Handing it to the
    // parser would corrupt the message after it, so it is counted and dropped.
    if ((packed & 0x80u) == 0) {
        dropped_messages_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    engine::Engine* engine = engine_;
    if (engine != nullptr)
        parser_.set_tick(engine->sample_position());
    const auto event = parser_.feed_packed(packed);

    // The counters are atomics because the application thread reads them while this thread
    // writes them; the parser itself stays owned by this thread.
    events_.store(parser_.events_out(), std::memory_order_relaxed);
    bad_bytes_.store(parser_.bad_bytes(), std::memory_order_relaxed);
    sysex_bytes_.store(parser_.sysex_bytes(), std::memory_order_relaxed);

    if (event && engine != nullptr)
        engine->push_event(*event);
}


} // namespace boreaz::io
