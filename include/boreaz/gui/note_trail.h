#pragma once

#include <cstdint>

#include "boreaz/gui/frame_view.h"

namespace boreaz::gui {

// The notes a strip scrolls: what is sounding, and what sounded a moment ago.
//
// The engine's telemetry is a photograph of now. Free a voice and it is gone from every counter, so
// a panel drawing that photograph shows a note vanish at the instant the key is let go - which is
// not what the ear hears, since the string rings on, and not what a transport looks like, since a
// bar that appears and disappears with the finger reads as a glitch rather than as music. So the
// history lives here instead: one entry per note, open while it sounds and closed with the second it
// stopped on, which is exactly what the piano roll and the waterfall are going to want to scroll and
// will not have to rebuild.
//
// It is built by watching one frame against the next, because the engine publishes no note events to
// the display - that is the plan's note-event ring, and it belongs to the piano roll, whose needs
// are wider (a song file's notes never touch a voice at all). Until then this is honest bookkeeping,
// not a poor substitute: the panel gets onsets the engine itself reported, from `Voice::age`.
class NoteTrail {
public:
    // How long a note is remembered. Longer than the strip's window by enough to have finished
    // scrolling out of it, and no longer: this is a fixed buffer, and a note nobody can see is
    // occupying the place of one that can still be played.
    static constexpr double kMemorySeconds = 12.0;

    // Reads `view.sounding` and writes `view.trail` and `view.trail_count`. Call it once per frame
    // after the transport clock, and not before: onsets are stored in transport seconds, so a note
    // played before the clock started has to be placed by the clock it will be scrolled past.
    void update(FrameView& view);

private:
    struct Entry {
        TrailNote note;
        bool seen = false;  // scratch: this frame's snapshot had it
    };

    // Oldest first, so dropping the oldest is dropping what has scrolled furthest left.
    Entry entries_[FrameView::kMaxTrail];
    int count_ = 0;
    double last_seconds_ = -1.0;
};

} // namespace boreaz::gui
