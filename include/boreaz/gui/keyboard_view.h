#pragma once

#include <cstddef>
#include <cstdint>

#include "boreaz/engine/event.h"
#include "boreaz/gui/frame_view.h"
#include "boreaz/gui/keyboard_layout.h"

namespace boreaz::gui {

// The 88 key strip, and the only place a mouse becomes a note.
//
// It produces events and does not push them: the app pushes into the engine, so a click and a
// cable travel the same path with the same meaning and cannot disagree about what a note is.
// Which key a click hit, and how hard, is KeyboardLayout's arithmetic, and that is where the
// tests live - this class is pixels and mouse buttons.
class KeyboardView {
  public:
    // Draw the strip where the layout says, and write up to `capacity` events the player caused
    // this frame: a press, a release, both across frames, or none. `view` says which notes are
    // sounding and how loudly, which is the only reason this takes a frame view at all.
    std::size_t draw(const FrameView& view, const KeyboardLayout& layout, std::int64_t tick,
                     std::uint8_t channel, boreaz::engine::Event* out, std::size_t capacity);

    // A key pressed here and not yet let go. Windows do not always report a release that happens
    // outside their own frame - losing focus, dragging off the edge - and a note left sounding
    // because the pointer escaped is the kind of bug that gets blamed on the synth. The app calls
    // this when the window stops being focused, and pushes what comes back.
    std::size_t force_release(std::int64_t tick, std::uint8_t channel,
                            boreaz::engine::Event* out, std::size_t capacity);

    int pressed_note() const { return pressed_note_; }
    // What the last click was aimed at, for the readout under the strip.
    int hovered_note() const { return hovered_note_; }

  private:
    int pressed_note_ = -1;
    int hovered_note_ = -1;
};

} // namespace boreaz::gui
