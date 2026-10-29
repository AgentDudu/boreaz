#pragma once

#include <initializer_list>  // for the two-candidate sweep in note_at

namespace boreaz::gui {

// A rectangle in window coordinates, float because that is what the window hands back. Not
// ImVec4: this file is the arithmetic that decides whether all 88 keys can be hit, and that is
// asserted in a test binary that links no window, no GPU, and no theme.
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    float right() const { return x + width; }
    float bottom() const { return y + height; }
    float center_x() const { return x + width * 0.5f; }
    bool contains(float px, float py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
};

// Where the 88 keys go, and which key a point is a click on.
//
// The strip is always the whole instrument at whatever width the window offers: the compass is
// fixed, so a narrow window buys narrow keys, not a scrollbar, a pager, or a zoom (plan 5). That
// is why "every one of the 88 is individually clickable" is a property of this geometry rather
// than something a user discovers at 1280 px, and it is why `playable()` says when the strip has
// stopped being honest.
//
// Nothing here knows about ImGui. The view draws these rectangles and turns a click into a note;
// everything that decides *which* note is here, where it can be tested without a display.
class KeyboardLayout {
  public:
    static constexpr int kFirstNote = 21;         // A0
    static constexpr int kLastNote = 108;         // C8
    static constexpr int kKeyCount = kLastNote - kFirstNote + 1; // 88
    static constexpr int kWhiteCount = 52;        // and 36 black, which is the other 36
    static constexpr int kMiddleC = 60;           // C4, the one the view labels
    static constexpr float kBlackWidthRatio = 0.6f;
    static constexpr float kBlackHeightRatio = 0.62f;

    KeyboardLayout() = default;
    KeyboardLayout(float x, float y, float width, float height)
        : x_(x), y_(y), width_(width), height_(height) {}

    float x() const { return x_; }
    float y() const { return y_; }
    float width() const { return width_; }
    float height() const { return height_; }
    Rect bounds() const { return Rect{x_, y_, width_, height_}; }
    float right() const { return x_ + width_; }
    float bottom() const { return y_ + height_; }
    float white_width() const { return width_ / static_cast<float>(kWhiteCount); }

    // False once a key is narrower than a pixel, at which point two adjacent keys answer the
    // same click and the view should draw a message instead of a keyboard it cannot play.
    bool playable() const {
        return width_ >= static_cast<float>(kKeyCount) && height_ >= 1.0f;
    }

    static bool is_black(int note) {
        switch (note % 12) {
        case 1:  // C#
        case 3:  // D#
        case 6:  // F#
        case 8:  // G#
        case 10: // A#
            return true;
        default:
            return false;
        }
    }
    static bool is_middle_c(int note) { return note == kMiddleC; }
    // Notes outside A0-C8 are not folded onto a neighbour key. The engine plays them and the
    // view says EXT, so a display can never disagree with what is sounding.
    static bool contains(int note) { return note >= kFirstNote && note <= kLastNote; }

    // This keyboard's own white key index, 0 for A0 .. 51 for C8.
    static int white_index_of(int note) { return naturals_below(note) - kFirstNatural; }
    // The note of the index-th white key, so the view can walk whites left to right and drop the
    // black keys in afterwards.
    static int note_at_white_index(int index) {
        static constexpr int kNaturalSteps[7] = {0, 2, 4, 5, 7, 9, 11};
        const int natural = index + kFirstNatural;
        return (natural / 7) * 12 + kNaturalSteps[natural % 7];
    }

    Rect white_rect(int note) const {
        const float unit = white_width();
        return Rect{x_ + static_cast<float>(white_index_of(note)) * unit, y_, unit, height_};
    }
    // Centred on the seam of the two whites it splits, which is why black keys overlap and why
    // the hit test tries them first. Only meaningful for a black note. The seam is the right edge
    // of the white below it, because naturals_below() counts that white as being under the black.
    Rect black_rect(int note) const {
        const Rect left_key = white_rect(note);
        const float black_width = left_key.width * kBlackWidthRatio;
        return Rect{left_key.right() - black_width * 0.5f, y_, black_width,
                    height_ * kBlackHeightRatio};
    }
    Rect rect_for(int note) const { return is_black(note) ? black_rect(note) : white_rect(note); }

    // The key a click belongs to, or -1 outside the strip. Where a black key overlaps two
    // whites, the black key wins, because that is what is drawn on top.
    int note_at(float px, float py) const {
        if (!bounds().contains(px, py))
            return -1;
        const int white = white_note_at(px);
        if (py < y_ + height_ * kBlackHeightRatio) {
            // A black key straddles one seam, so only its two neighbours can be under the
            // pointer: the black note on this white's left seam and the one on its right.
            for (const int candidate : {white - 1, white + 1}) {
                if (contains(candidate) && is_black(candidate) && black_rect(candidate).contains(px, py))
                    return candidate;
            }
        }
        return white;
    }

    // How hard the key was struck, from where: the top of the key bed is the far end of a real
    // key and the loudest place to hit one. Never 0, because MIDI reads note-on at velocity 0 as
    // a note-off, and a click at the bottom of a key must not be a silent misfire.
    static int velocity_at(float py, float top, float height) {
        if (height <= 0.0f)
            return 127;
        const float from_top = (py - top) / height;
        float scaled = 127.0f - from_top * (127.0f - kMinVelocity);
        if (scaled < kMinVelocity)
            scaled = kMinVelocity;
        if (scaled > 127.0f)
            scaled = 127.0f;
        return static_cast<int>(scaled + 0.5f);
    }
    int velocity_at(float py) const { return velocity_at(py, y_, height_); }

  private:
    static constexpr float kMinVelocity = 1.0f;
    // A0 is the 12th natural of the MIDI scale (C0 counts as the first), which is all the offset
    // the two index conversions above need.
    static constexpr int kFirstNatural = 12;

    // How many natural notes are strictly below `note`, counting C0 as the first.
    static int naturals_below(int note) {
        static constexpr int kNaturalsBelow[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};
        return (note / 12) * 7 + kNaturalsBelow[note % 12];
    }

    int white_note_at(float px) const {
        const float unit = white_width();
        int index = unit > 0.0f ? static_cast<int>((px - x_) / unit) : 0;
        if (index < 0)
            index = 0;
        if (index >= kWhiteCount)
            index = kWhiteCount - 1;
        return note_at_white_index(index);
    }

    float x_ = 0.0f;
    float y_ = 0.0f;
    float width_ = 0.0f;
    float height_ = 0.0f;
};

} // namespace boreaz::gui
