#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "boreaz/engine/engine.h"

namespace boreaz::gui {

// One sounding note, as the display sees it.
struct Sounding {
    std::uint8_t note = 0;
    std::uint8_t key = 0;     // the key that produced it, before transpose
    std::uint8_t channel = 0;
    std::uint8_t velocity = 0; // 0-127, as it arrived
    bool held = false;         // the key is still up: false when only the pedal is holding it
    float level = 0.0f;        // 0..1, what a key or a lane is drawn with
    // How long this voice has been sounding, in samples, taken at the same position as
    // FrameView::sample_position. A strip that draws where a note started has to know when it
    // started, and the voice already computes it; without this a transport can only show that
    // something is playing, never that it started somewhere behind the playhead.
    std::int64_t age_samples = 0;
};

// One note a strip scrolls - sounding or just stopped - placed by the transport's clock, which is
// the only clock two notes can agree on. `end_seconds` is negative while the note is still
// sounding, which is what tells a bar that is growing from a bar that has finished and is merely
// travelling left: the engine forgets a voice when it is freed, so "it stopped" has to be a fact
// the display keeps.
struct TrailNote {
    std::uint8_t note = 0;    // the note as it sounds - where the strip draws it
    std::uint8_t key = 0;     // the key it came in on: how two frames know this is the same note
    std::uint8_t channel = 0;
    std::uint8_t velocity = 0;  // the touch it started on, which never changes again
    float level = 0.0f;         // the last level reported while it sounded
    double onset_seconds = 0.0;
    double end_seconds = -1.0;  // negative while sounding

    bool sounding() const { return end_seconds < 0.0; }
};

// Device and port numbers the window shows. Plain data, because the window must not learn to
// open a device: `app` reads its own objects and hands the numbers over.
struct DeviceStats {
    bool open = false;
    double sample_rate = 0.0;
    int callback_frames = 0;
    double cpu_fraction = 0.0;
    std::uint64_t callbacks = 0;
    std::uint64_t late_callbacks = 0;
    std::uint64_t restarts = 0;
    std::string name;
};

struct MidiStats {
    bool open = false;
    std::uint64_t losses = 0;
    // Messages the port has delivered since it opened, which is how "it went quiet" and "nothing
    // was ever plugged in" are two different sentences on the strip.
    std::uint64_t events = 0;
    std::string name;
};

// Everything a panel is allowed to know about the running instrument, in one POD that `app`
// copies together once per frame from the engine's telemetry.
//
// A panel does not read the engine. The engine's own documentation is that one thread produces
// and one consumes, and its telemetry is a handful of relaxed scalars, not a consistent picture:
// reaching into a live allocator mid-frame would be a display that can delay the audio path, and
// the plan makes that a bug (plan 3). One copy per frame, drawn all frame long.
struct FrameView {
    static constexpr int kMaxVoices = static_cast<int>(boreaz::engine::Engine::kMaxPolyphony);
    // Enough notes to fill the strip's window at a glissando, and no more: the ones past the window
    // are off the left edge, and a fixed buffer is what keeps a frame allocation-free.
    static constexpr int kMaxTrail = 96;

    // Where the audio thread is.
    DeviceStats device;
    MidiStats midi;

    std::int64_t sample_position = 0;
    // Before the limiter, so 1.0 is where the tanh starts bending: this is the clip meter, and
    // the engine already computed it once per block for exactly this purpose.
    float drive_peak = 0.0f;
    int active_voices = 0;
    int pool_size = 0;
    std::uint64_t steals = 0;
    std::size_t dropped_events = 0;
    bool sustain = false;

    // The transport, not the audio clock. The audio thread counts samples from the moment the device
    // opens, which is not when music starts, so a panel that prints that number shows a stopwatch
    // where a player expects a song position. Both fields come from TransportClock: not running and
    // zero seconds until a song is loaded or a note is played, and parked at the last value when the
    // song ends.
    bool transport_running = false;
    double transport_seconds = 0.0;

    // The notes themselves, plus the ones this keyboard cannot show. A voice playing note 9 is
    // real and audible, and an 88 key display that silently drops it is lying about the pitch, so
    // it counts here and the view says EXT.
    int sounding_count = 0;
    int ext_voices = 0;
    Sounding sounding[static_cast<std::size_t>(kMaxVoices)]{};

    // The notes the panels scroll, oldest first: the ones sounding now plus the ones that stopped
    // too recently to have left the strip. `NoteTrail` fills these - it is the one part of the
    // display with memory between frames, and so the one place a note that has ended still exists.
    TrailNote trail[static_cast<std::size_t>(kMaxTrail)]{};
    int trail_count = 0;

    // Window throughput, because "the window at 60 fps while audio runs" is an exit criterion and
    // a number someone has to be able to quote without a profiler.
    double frames = 0.0;
    double seconds = 0.0;

    double position_seconds() const {
        return device.sample_rate > 0.0 ? static_cast<double>(sample_position) / device.sample_rate
                                        : 0.0;
    }
    double frames_per_second() const { return seconds > 0.0 ? frames / seconds : 0.0; }
};

// Copy the telemetry out. Runs on the UI thread, reads only what the engine says the UI may read.
FrameView make_frame_view(const boreaz::engine::Engine& engine, DeviceStats device,
                          MidiStats midi);

} // namespace boreaz::gui
