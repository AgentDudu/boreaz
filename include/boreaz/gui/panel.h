#pragma once

#include <cstdint>

#include "boreaz/gui/frame_view.h"

namespace boreaz::gui {

// The switchable panel that sits above the keyboard (plan 1). One value in this enum, one row in
// the table in src/gui/panel.cpp, one file in src/gui/viz/: that is the entire cost of adding a
// visualiser, and the static_assert next to the table is what makes the claim true instead of
// aspirational.
enum class PanelKind : std::uint8_t {
    kTransport = 0,
    kCount, // sentinel: the table has to have exactly this many rows
};

// A panel draws itself at the cursor of the child window the app made for it, and reads nothing
// but the FrameView it was handed. No engine pointer, no device, no way to delay audio.
using PanelDrawFn = void (*)(const FrameView&);

struct PanelEntry {
    PanelKind kind;
    const char* label; // what the switcher button says
    const char* hint;  // the tooltip, because "Transport" alone does not say it cannot play yet
    PanelDrawFn draw;
};

const PanelEntry& panel(PanelKind kind);
const PanelEntry* panel_table(int* count_out);

} // namespace boreaz::gui
