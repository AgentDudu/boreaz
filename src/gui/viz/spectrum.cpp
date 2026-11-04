// The spectrum panel: what the air is doing, cut into the instrument's 88 key ranges.
//
// It draws levels the engine never produces. They come from the master bus, measured in the app's frame
// loop and handed over as numbers in the FrameView, exactly like the device counters: this file has no
// analyser, no device, and no way to delay a block (plan 3). What it can do is print what it was given,
// including what was dropped on the way to it.
//
// The x geometry is KeyboardLayout asked for the same width as the keyboard below, so a band sits over
// the key that plays it. That is the whole argument for cutting the spectrum by key range instead of by
// log frequency: the spectrum and the waterfall above the same keyboard then agree about where a note is,
// because they are the same ruler twice.
//
// Levels are dB relative to full scale, floor at -120, which is the only absolute the master bus has.

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "boreaz/gui/frame_view.h"
#include "boreaz/gui/keyboard_layout.h"

namespace boreaz::gui {
namespace viz {
namespace {

const ImVec4 kAccent(0.45f, 0.86f, 0.72f, 1.0f);
const ImVec4 kRuler(1.0f, 1.0f, 1.0f, 0.10f);
const ImVec4 kDim(0.62f, 0.66f, 0.62f, 1.0f);

// The dB grid. Not every 6 dB: four lines is what fits beside 88 bands and still says where -20 is.
const float kDbLines[] = {-60.0f, -40.0f, -20.0f, 0.0f};

const char* const kWindowNames[] = {"2048 samples (1179 bands)", "8192 samples (295 bands)"};

} // namespace

void spectrum(const FrameView& view) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area_size = ImGui::GetContentRegionAvail();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // The panel's own look, held here and nowhere else, like every other panel.
    static int window_choice = 1; // index into the two the app measures
    static bool show_hold = true;

    const float row = ImGui::GetTextLineHeightWithSpacing();
    const float plot_top = origin.y + row + 6.0f;
    const float plot_bottom = origin.y + area_size.y;
    const float plot_height = plot_bottom - plot_top;
    const bool enough = area_size.x >= 88.0f && plot_height >= 24.0f;

    const bool wide = window_choice == 1;
    const float* db = (wide ? view.spectrum.wide_db : view.spectrum.fast_db).data();
    const float* hold = (wide ? view.spectrum.wide_peak : view.spectrum.fast_peak).data();
    const double usec = wide ? view.spectrum.wide_microseconds : view.spectrum.fast_microseconds;
    const int size = wide ? view.spectrum.wide_size : view.spectrum.fast_size;

    ImGui::SetCursorScreenPos(origin);
    ImGui::Combo("Window", &window_choice, kWindowNames[0]);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "One window cannot do both jobs. The short one lights a transient in the frame after it "
            "happens, and cannot tell A0 from A#0. The long one separates the low notes and smears a "
            "transient over a tenth of a second. The bands are the same 88 key ranges either way.");
    ImGui::SameLine();
    ImGui::Checkbox("Hold", &show_hold);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The loudest each band has been recently, falling at the engine's own decay "
                          "rate. A hammer strikes once and is gone; a bar that has already fallen to "
                          "the average hides that it happened at all.");
    ImGui::SameLine();
    char cost[64];
    std::snprintf(cost, sizeof cost, "%d samples  \xc2\xb7  transform %.1f ms", size, usec * 1e-3);
    ImGui::TextUnformatted(cost);

    if (!enough) {
        const char* too_narrow = "too narrow for 88 bands";
        draw->AddText(ImVec2(origin.x + 4.0f, plot_top + 4.0f), ImGui::GetColorU32(kDim), too_narrow);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, plot_bottom));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        return;
    }

    const float floor_db = SpectrumSnapshot::kFloorDb;
    const auto y_for = [&](float level) {
        const float t = (level - floor_db) / (0.0f - floor_db);
        return plot_bottom - std::clamp(t, 0.0f, 1.0f) * plot_height;
    };

    // The dB grid, and the C lines: the ruler is drawn first so a bar is always the brightest thing
    // in its column, as in the waterfall.
    for (float db_line : kDbLines) {
        const float y = y_for(db_line);
        draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + area_size.x, y),
                      ImGui::GetColorU32(kRuler), 1.0f);
        char label[8];
        std::snprintf(label, sizeof label, "%d", static_cast<int>(db_line));
        draw->AddText(ImVec2(origin.x + 2.0f, y - ImGui::GetFontSize() - 1.0f),
                      ImGui::GetColorU32(kDim), label);
    }

    const KeyboardLayout keys(origin.x, plot_top, area_size.x, plot_height);
    bool silent = true;
    for (int note = KeyboardLayout::kFirstNote; note <= KeyboardLayout::kLastNote; ++note) {
        const std::size_t band_index = static_cast<std::size_t>(note - KeyboardLayout::kFirstNote);
        const Rect band = keys.rect_for(note);
        if (note % 12 == 0)
            draw->AddLine(ImVec2(band.x, plot_top), ImVec2(band.x, plot_bottom),
                          ImGui::GetColorU32(kRuler), 1.0f);
        const float level = db[band_index];
        if (level > floor_db + 0.5f)
            silent = false;
        const float t = (level - floor_db) / (0.0f - floor_db);
        if (t <= 0.002f)
            continue;
        // One hue, brighter as the band gets louder: the colour carries level here, not channel,
        // because this view is about the air and not about who sent it. The channel colours belong to
        // the waterfall, where the same song is already drawn in them.
        const float alpha = 0.40f + 0.60f * t;
        const float top = y_for(level);
        draw->AddRectFilled(ImVec2(band.x, top), ImVec2(band.right(), plot_bottom),
                            ImGui::GetColorU32(ImVec4(kAccent.x, kAccent.y, kAccent.z, alpha)));
        if (show_hold) {
            const float h = hold[band_index];
            if (h > level)
                draw->AddLine(ImVec2(band.x, y_for(h)), ImVec2(band.right(), y_for(h)),
                              ImGui::GetColorU32(kAccent), 1.5f);
        }
    }

    if (silent) {
        const char* waiting = "no audio to measure";
        draw->AddText(ImVec2(origin.x + (area_size.x - 150.0f) * 0.5f,
                             plot_top + (plot_height - ImGui::GetFontSize()) * 0.5f),
                      ImGui::GetColorU32(kDim), waiting);
    }

    // What the measurement is missing. A tap that drops frames is the frame loop being slower than the
    // sound, and a panel that shows a hole without saying so is a meter with a blind spot.
    if (view.spectrum.dropped_frames != 0u || view.spectrum.unanalysed_frames != 0u) {
        char holes[96];
        std::snprintf(holes, sizeof holes,
                      "%llu frames of sound never copied  \xc2\xb7  %llu older than the window",
                      static_cast<unsigned long long>(view.spectrum.dropped_frames),
                      static_cast<unsigned long long>(view.spectrum.unanalysed_frames));
        draw->AddText(ImVec2(origin.x + 4.0f, plot_bottom - ImGui::GetFontSize() - 2.0f),
                      ImGui::GetColorU32(kDim), holes);
    }

    // The panel consumed its region: without this the child window sizes itself to the control row and
    // clips the plot, the same trap the other two panels document.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, plot_bottom));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}

} // namespace viz
} // namespace boreaz::gui
