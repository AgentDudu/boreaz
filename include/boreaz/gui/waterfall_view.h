#pragma once

#include <cstdint>

#include "boreaz/gui/frame_view.h"
#include "boreaz/gui/keyboard_layout.h"

namespace boreaz::gui {

// One note in the waterfall: a column as wide as the key that plays it, and a height that is the
// time it sounded.
struct WaterfallBar {
    Rect rect;
    std::uint8_t note = 0;
    std::uint8_t channel = 0;
    float level = 0.0f;    // what the note is worth right now, 0..1
    bool sounding = false; // still touching the keyboard line, so still growing
};

// The geometry of the note waterfall: 88 columns, one per key, with time running upwards.
//
// A lane's x is `KeyboardLayout`'s own rectangle for that note, asked of the same width the
// keyboard below is drawn at. There is one arithmetic and both views call it, which is what turns
// "the lanes line up with the keys to the pixel" from an eye test into an assertion.
//
// The vertical axis is the transport clock - the same clock the transport strip scrolls by. The
// bottom edge of the area is the keyboard line: a note starts there, grows upwards while it is
// held, and then travels up and out of sight at its own length. Nothing falls in from the top,
// because the display has no look-ahead: the FrameView is a photograph of what the engine already
// started (plan 3), and a note arriving from above would be a note the player had not heard yet.
//
// No ImGui and no draw list. The alignment criterion, the 5 ms staccato, and the rule that a real
// note is never dropped are all asserted here, in a binary with no window and no GPU.
class WaterfallView {
  public:
    // The plan's scroll-speed control, in pixels of height per second of music.
    static constexpr float kMinScroll = 12.0f;
    static constexpr float kMaxScroll = 320.0f;
    static constexpr float kDefaultScroll = 96.0f;

    // The plan's lane-height control: the shortest bar the view draws. A staccato note off a cable
    // is a few milliseconds, which at any readable scroll speed is a fraction of a pixel, and a
    // note the ear heard that the display refuses to draw is a display disagreeing with the ear.
    static constexpr float kMinLaneHeight = 1.0f;
    static constexpr float kMaxLaneHeight = 12.0f;
    static constexpr float kDefaultLaneHeight = 2.0f;

    WaterfallView() = default;

    // `area` is the panel's own rectangle. Its x and width are the keyboard's, and its bottom edge
    // is the keyboard line that notes leave - which is why the area is passed in rather than a
    // height being decided here.
    WaterfallView(const Rect& area, float scroll, float lane_height);

    // Where `note` sits at `now`, on the transport clock. False means the bar is not on the area:
    // scrolled out above the top, or a note outside A0 to C8, which a view of 88 lanes cannot
    // place and the caller counts out loud - an 88-column display that silently drops note 9 is
    // lying about the pitch.
    bool bar_for(const TrailNote& note, double now, WaterfallBar* out) const;

    // How much music the area's height covers. The panel prints it beside the scroll control so the
    // number being set is in the unit the player is thinking in.
    double visible_seconds() const;

    const KeyboardLayout& keys() const { return keys_; }
    float scroll() const { return scroll_; }
    float lane_height() const { return lane_height_; }

  private:
    KeyboardLayout keys_;
    float scroll_ = kDefaultScroll;
    float lane_height_ = kDefaultLaneHeight;
};

} // namespace boreaz::gui
