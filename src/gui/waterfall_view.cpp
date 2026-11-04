// The waterfall's arithmetic: which column a note occupies, and how tall its bar is right now.
//
// Two facts decide every bar. The column is the key's own rectangle, so the lane and the key cannot
// disagree. The height is elapsed time: a note's start sits `now - onset` seconds above the
// keyboard line, and its newest end sits at the line itself while it is still sounding, or `now -
// end` seconds above it once it has stopped. Everything else - colour, fade, the playhead line - is
// the view's drawing policy and lives in src/gui/viz/waterfall.cpp.
//
// A bar is clipped to the area rather than dropped when it crosses an edge, because a note that is
// half on screen is still sounding. The only note this refuses to draw is one that has scrolled
// entirely out of sight at the top, or one outside the instrument: the caller counts the second
// case, because an 88-lane view that quietly ignores note 9 would be a view the player cannot
// trust.

#include "boreaz/gui/waterfall_view.h"

#include <algorithm>

namespace boreaz::gui {

WaterfallView::WaterfallView(const Rect& area, float scroll, float lane_height)
    : keys_(area.x, area.y, area.width, area.height),
      scroll_(std::clamp(scroll, kMinScroll, kMaxScroll)),
      lane_height_(std::clamp(lane_height, kMinLaneHeight, kMaxLaneHeight)) {}

double WaterfallView::visible_seconds() const {
    return scroll_ > 0.0f ? static_cast<double>(keys_.height()) / static_cast<double>(scroll_)
                          : 0.0;
}

bool WaterfallView::bar_for(const TrailNote& note, double now, WaterfallBar* out) const {
    if (out == nullptr || !KeyboardLayout::contains(note.note))
        return false;

    // A note whose onset is ahead of the clock exists: Backspace sets the strip's clock back to the
    // playhead while the trail still holds what was played under the old one. Taking the age as
    // zero draws it at the line instead of above where it has not happened yet.
    const double onset_age = std::max(0.0, now - note.onset_seconds);
    const double newest_age = note.sounding() ? 0.0 : std::max(0.0, now - note.end_seconds);

    const float bottom = keys_.bottom();
    const float top_edge = keys_.y();

    // The newest end of the bar leaves through the top. When that has gone, the whole note has.
    const float newest = bottom - static_cast<float>(newest_age) * scroll_;
    if (newest < top_edge)
        return false;

    float high = std::max(top_edge, bottom - static_cast<float>(onset_age) * scroll_);
    float low = std::min(bottom, newest);

    // The floor is applied by growing upwards, never by pushing the bar below the line: a note that
    // was struck this instant is one pixel tall *above* the keys, which is what "I heard it" means.
    if (low - high < lane_height_) {
        high = low - lane_height_;
        if (high < top_edge)
            high = top_edge;
    }
    if (low <= high)
        return false; // an area shorter than the floor cannot show anything

    const Rect key = keys_.rect_for(note.note);
    out->rect = Rect{key.x, high, key.width, low - high};
    out->note = note.note;
    out->channel = note.channel;
    out->level = note.level;
    out->sounding = note.sounding();
    return true;
}

} // namespace boreaz::gui
