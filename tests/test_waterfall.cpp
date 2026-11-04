// The waterfall's geometry: 88 columns that have to sit over their keys, and a note that has to be
// drawn at whatever length it had.
//
// No window and no GPU: every criterion about alignment, about a staccato note still appearing, and
// about notes being dropped is arithmetic, and arithmetic asserted here is arithmetic that cannot
// regress on a machine with no display. What is not tested here is what only a live window knows -
// frames per second, and whether the audio thread stayed quiet - and docs/ says so rather than
// implying these tests cover it.

#include "boreaz/gui/waterfall_view.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

namespace {

using boreaz::gui::FrameView;
using boreaz::gui::KeyboardLayout;
using boreaz::gui::Rect;
using boreaz::gui::TrailNote;
using boreaz::gui::WaterfallBar;
using boreaz::gui::WaterfallView;
using doctest::Approx;

// A note the trail has already closed, at an explicit place on the transport clock.
TrailNote note_at(int note, double onset, double end, std::uint8_t channel = 0) {
    TrailNote out;
    out.note = static_cast<std::uint8_t>(note);
    out.key = out.note;
    out.channel = channel;
    out.velocity = 100;
    out.level = 0.7f;
    out.onset_seconds = onset;
    out.end_seconds = end; // negative keeps it sounding, which the trail does the same way
    return out;
}

// The plot the app would hand the view: the panel's width is the keyboard's width, because both
// come from the same content region (see AppWindow::draw).
constexpr float kX = 12.0f;
constexpr float kWidth = 1428.0f;
constexpr float kPanelTop = 60.0f;
constexpr float kPanelHeight = 380.0f;

Rect panel_area() {
    return Rect{kX, kPanelTop, kWidth, kPanelHeight};
}

} // namespace

TEST_CASE("every waterfall lane sits over its own key") {
    // The claim the plan states as "to the pixel", asserted as exactly equal floats and not with a
    // tolerance: a lane and a key come out of the same KeyboardLayout, so anything but
    // bit-identical numbers would mean two pieces of arithmetic had been written instead of one.
    for (const float width : {88.0f, 480.0f, 1428.0f, 2548.0f}) {
        for (const float x : {0.0f, 12.0f, 137.5f}) {
            // The keyboard sits under the panel in the app; only its y differs.
            const KeyboardLayout keys(x, 500.0f, width, 180.0f);
            const WaterfallView view(Rect{x, kPanelTop, width, kPanelHeight},
                                     WaterfallView::kDefaultScroll,
                                     WaterfallView::kDefaultLaneHeight);
            for (int note = KeyboardLayout::kFirstNote; note <= KeyboardLayout::kLastNote; ++note) {
                const TrailNote played = note_at(note, 1.0, 1.5);
                WaterfallBar bar;
                REQUIRE(view.bar_for(played, 2.0, &bar));
                const Rect key = keys.rect_for(note);
                CHECK(bar.rect.x == key.x);
                CHECK(bar.rect.width == key.width);
                INFO("note " << note << " at width " << width << ", x " << x);
            }
            CHECK(keys.white_width() == view.keys().white_width());
        }
    }
}

TEST_CASE("a five millisecond staccato note is still a bar") {
    // The slowest scroll speed the control offers, so this is the worst case: 5 ms is 0.06 px of
    // travel. The bar has to be the lane-height floor instead, and it has to be above the keyboard
    // line and not through it.
    const WaterfallView view(panel_area(), WaterfallView::kMinScroll,
                             WaterfallView::kDefaultLaneHeight);
    WaterfallBar bar;
    REQUIRE(view.bar_for(note_at(60, 10.0, 10.005), 10.005, &bar));
    CHECK(bar.rect.height >= view.lane_height());
    CHECK(bar.rect.bottom() == Approx(440.0f)); // the plot's bottom edge is the keyboard line
    CHECK(bar.rect.y >= kPanelTop);
    INFO("a bar of " << bar.rect.height << " px at " << view.scroll() << " px/s");
}

TEST_CASE("a fresh note grows upwards out of the keys and never below them") {
    const WaterfallView view(panel_area(), WaterfallView::kDefaultScroll, 6.0f);
    WaterfallBar bar;
    REQUIRE(view.bar_for(note_at(64, 3.0, -1.0), 3.0, &bar)); // struck this frame, sounding
    CHECK(bar.rect.bottom() == Approx(440.0f));
    CHECK(bar.rect.height == Approx(6.0f));
    CHECK(bar.sounding);
}

TEST_CASE("forty notes sounding at once all get a lane, and none is dropped") {
    // The plan's concurrency figure. The trail holds 96 notes, and nothing in the geometry is
    // allowed to lose one of them: this is the display's own budget, separate from the engine's
    // polyphony. The trail's own capacity, checked at compile time: if it held fewer than the
    // plan's concurrency figure, no amount of geometry could keep 40 notes on screen.
    static_assert(FrameView::kMaxTrail >= 40,
                  "the trail must hold at least the concurrency the waterfall is judged on");
    const WaterfallView view(panel_area(), WaterfallView::kDefaultScroll,
                             WaterfallView::kDefaultLaneHeight);
    std::vector<WaterfallBar> bars;
    for (int index = 0; index < 40; ++index) {
        const int note = KeyboardLayout::kFirstNote + index * 2; // every other key, 21..99
        WaterfallBar bar;
        REQUIRE(view.bar_for(note_at(note, 0.5, -1.0, static_cast<std::uint8_t>(index % 16)), 1.5,
                             &bar));
        bars.push_back(bar);
    }
    CHECK(bars.size() == 40u);
    for (const WaterfallBar& bar : bars) {
        CHECK(bar.rect.height > 0.0f);
        CHECK(bar.rect.bottom() <= Approx(440.0f));
        CHECK(bar.rect.y >= Approx(kPanelTop));
    }
    // A note a second old is a second tall at this scroll speed: the bar is the whole time the note
    // has sounded, measured from the line it started at and not from the frame it is drawn in.
    CHECK(bars.front().rect.height == Approx(1.0f * WaterfallView::kDefaultScroll));
}

TEST_CASE("a note is clipped at the edges of the plot and dropped only when it has left") {
    const WaterfallView view(panel_area(), WaterfallView::kDefaultScroll,
                             WaterfallView::kDefaultLaneHeight);
    const double visible = view.visible_seconds(); // the height of the plot, in seconds
    CHECK(visible == Approx(kPanelHeight / WaterfallView::kDefaultScroll));

    // Held longer than the plot can show: the top of the bar is cut off, the note is still
    // sounding, so it is still on screen.
    WaterfallBar held;
    REQUIRE(view.bar_for(note_at(60, 0.0, -1.0), visible * 2.0, &held));
    CHECK(held.rect.y == Approx(kPanelTop));
    CHECK(held.rect.height > 0.0f);

    // Ended long enough ago that its newest end has gone past the top: gone, and the caller stops
    // drawing it.
    WaterfallBar gone;
    CHECK_FALSE(view.bar_for(note_at(60, 0.0, visible * 3.0), visible * 4.0, &gone));
}

TEST_CASE("a note the 88 lanes cannot place is refused and can be counted") {
    const WaterfallView view(panel_area(), WaterfallView::kDefaultScroll,
                             WaterfallView::kDefaultLaneHeight);
    WaterfallBar bar;
    for (const int outside : {0, 9, 20, 109, 127}) {
        INFO("note " << outside);
        CHECK_FALSE(KeyboardLayout::contains(outside));
        CHECK_FALSE(view.bar_for(note_at(outside, 1.0, 2.0), 2.0, &bar));
    }
    // The instrument's own two ends are placed, so the refusal is about the compass and not about
    // the arithmetic near it.
    for (const int edge : {KeyboardLayout::kFirstNote, KeyboardLayout::kLastNote}) {
        INFO("note " << edge);
        REQUIRE(view.bar_for(note_at(edge, 1.0, 2.0), 2.0, &bar));
        CHECK(bar.rect.width > 0.0f);
    }
}

TEST_CASE("the clock going backwards does not put a note above where it has not happened") {
    // Backspace sets the strip's clock back to the playhead while the trail still holds notes from
    // under the old one. A note whose onset is ahead of the clock is drawn at the keyboard line,
    // which is what "now" means, rather than floating in the past-of-the-future.
    const WaterfallView view(panel_area(), WaterfallView::kDefaultScroll,
                             WaterfallView::kDefaultLaneHeight);
    WaterfallBar bar;
    REQUIRE(view.bar_for(note_at(64, 30.0, -1.0), 2.0, &bar));
    CHECK(bar.rect.bottom() == Approx(440.0f));
    CHECK(bar.rect.y >= Approx(kPanelTop));
}

TEST_CASE("the two controls move the picture and cannot be set somewhere invalid") {
    WaterfallBar bar;
    const TrailNote played = note_at(60, 0.0, 0.25);

    const WaterfallView slow(panel_area(), WaterfallView::kMinScroll,
                             WaterfallView::kMinLaneHeight);
    const WaterfallView fast(panel_area(), WaterfallView::kMaxScroll,
                             WaterfallView::kMinLaneHeight);
    REQUIRE(slow.bar_for(played, 0.5, &bar));
    const float slow_height = bar.rect.height;
    REQUIRE(fast.bar_for(played, 0.5, &bar));
    CHECK(bar.rect.height > slow_height); // more pixels per second is a taller bar
    CHECK(fast.visible_seconds() < slow.visible_seconds());

    // Out of range at both ends: the view clamps to what it can draw instead of taking a scroll
    // speed that puts every note off screen, or a lane height of zero that draws nothing.
    const WaterfallView nonsense(panel_area(), -100.0f, 1000.0f);
    CHECK(nonsense.scroll() == WaterfallView::kMinScroll);
    CHECK(nonsense.lane_height() == WaterfallView::kMaxLaneHeight);
    REQUIRE(nonsense.bar_for(played, 0.5, &bar));
    CHECK(bar.rect.height >= WaterfallView::kMinLaneHeight);
}
