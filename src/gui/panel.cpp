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
} // namespace viz

namespace {

constexpr PanelEntry kTable[] = {
    {PanelKind::kTransport, "Transport",
     "Where the song is: elapsed time, the beat grid, the playhead. A readout - the keys and the "
     "space bar play, this strip only tells you where the playback has got to.",
     &viz::transport},
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
