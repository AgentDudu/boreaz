// The panel table: the one place that knows which visualisers exist.
//
// One row per PanelKind, and the static_assert below is what turns "add a panel by adding a file in
// src/gui/viz/" from a hope into a build failure: a new value in the enum with no row here stops at
// compile time instead of returning the wrong panel at run time.

#include "boreaz/gui/panel.h"

#include <cstddef>

namespace boreaz::gui {

namespace viz {
void transport(const FrameView& view);
void waterfall(const FrameView& view);
} // namespace viz

namespace {

constexpr PanelEntry kTable[] = {
    {PanelKind::kTransport, "Transport",
     "Where the song is: a strip that sits still until a note or a song starts it and then rolls, "
     "the playhead at the right, two meters at its foot. The row above this one is the player's "
     "own: play, stop, loop, a place to jump to, and a song chosen without restarting the app. "
     "Backspace sets the clock back to zero.",
     &viz::transport},
    {PanelKind::kWaterfall, "Waterfall",
     "What has been played, in the columns of the keys that played it. Time runs upwards from the "
     "keyboard line: a note starts there, grows while it is held, and leaves at the length it "
     "sounded. Height is how long, brightness is how hard, colour is which channel. Scroll and "
     "lane "
     "height are this view's own, and F2 moves to the next view.",
     &viz::waterfall},
};

static_assert(std::size(kTable) == static_cast<std::size_t>(PanelKind::kCount),
              "one row in the panel table per PanelKind value; the switcher and the keyboard "
              "shortcut walk this table, so a missing row is an out-of-range read, not a "
              "compile error anywhere else");

} // namespace

const PanelEntry& panel(PanelKind kind) {
    const int index = static_cast<int>(kind);
    // A value outside the enum arrives only from a PanelKind that grew without a row, which the
    // assert above already forbids; clamping keeps a future refactor from reading past the table.
    if (index < 0 || index >= static_cast<int>(std::size(kTable)))
        return kTable[0];
    return kTable[static_cast<std::size_t>(index)];
}

const PanelEntry* panel_table(int* count_out) {
    if (count_out != nullptr)
        *count_out = static_cast<int>(std::size(kTable));
    return kTable;
}

} // namespace boreaz::gui
