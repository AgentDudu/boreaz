// The note waterfall: what has been played, in the columns of the keys that played it.
//
// Time runs upwards. The bottom edge of the area is the keyboard line, a note starts there, grows
// while it is held, and then travels up at the length it sounded and leaves through the top.
// Nothing enters from the top because the display has no look-ahead - the FrameView is a photograph
// of what the engine already started - so the whole view is history, which is what a player looks
// at after a run to see which notes were not even there.
//
// It reads the same two numbers the transport strip does and nothing else: no engine pointer, no
// device, no way to delay a block (plan 3). The x of every lane comes from KeyboardLayout asked for
// the same width the keyboard below is drawn at, so the waterfall above the keys and the keys under
// it are the same geometry twice rather than two drawings that hope to agree.
//
// It draws into whatever rectangle the app gave it and fills it, like every other panel.

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "boreaz/gui/waterfall_view.h"

namespace boreaz::gui {
namespace viz {
namespace {

// One colour per MIDI channel. The transport strip draws every note the same colour because it
// shows one thing at a time; the waterfall is the only view in which the channel a note arrived on
// is visible at all, which is worth sixteen colours and costs nothing but a lookup. A song on one
// channel is one colour, and that is the truth rather than a limitation of the drawing.
//
// Distinct at a glance rather than pretty: hue and lightness both move between neighbours, because
// two channels sounding at once in a multi-channel file have to be told apart while both hands are
// busy.
const ImVec4 kChannelColour[16] = {
    ImVec4(0.45f, 0.86f, 0.72f, 1.0f), ImVec4(0.95f, 0.62f, 0.24f, 1.0f),
    ImVec4(0.55f, 0.68f, 0.98f, 1.0f), ImVec4(0.96f, 0.48f, 0.52f, 1.0f),
    ImVec4(0.72f, 0.90f, 0.42f, 1.0f), ImVec4(0.94f, 0.82f, 0.35f, 1.0f),
    ImVec4(0.62f, 0.52f, 0.94f, 1.0f), ImVec4(0.42f, 0.82f, 0.92f, 1.0f),
    ImVec4(0.98f, 0.66f, 0.78f, 1.0f), ImVec4(0.58f, 0.78f, 0.66f, 1.0f),
    ImVec4(0.86f, 0.72f, 0.94f, 1.0f), ImVec4(0.98f, 0.74f, 0.44f, 1.0f),
    ImVec4(0.48f, 0.74f, 0.80f, 1.0f), ImVec4(0.90f, 0.56f, 0.64f, 1.0f),
    ImVec4(0.66f, 0.86f, 0.58f, 1.0f), ImVec4(0.82f, 0.62f, 0.70f, 1.0f),
};

// How wide the band a note leaves through is, at the very top of the area. Fixed pixels rather than
// a fraction of the height: the fade wants to be the same physical size at every window height, and
// a fraction of a tall plot put the death of a note a third of the way from the edge it was still
// travelling to.
constexpr float kFadePixels = 20.0f;

// The scroll-speed bar's width. Wide enough that the fill reads as a length of past, narrow enough
// that the lane-height control and the on-screen span still fit the line.
constexpr float kScrollBarWidth = 200.0f;

// The line the notes leave, and the octave marks. Kept thin and dim: they are the ruler, and the
// notes are what is being read.
const ImVec4 kAccent(0.45f, 0.86f, 0.72f, 1.0f);
const ImVec4 kRuler(1.0f, 1.0f, 1.0f, 0.10f);
const ImVec4 kDim(0.62f, 0.66f, 0.62f, 1.0f);

} // namespace

void waterfall(const FrameView& view) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area_size = ImGui::GetContentRegionAvail();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // The view's own look, held here and nowhere else: the panel has no way to reach the app and
    // the app has no reason to know what a waterfall is scaled at. The panel is a view; this is the
    // only state it keeps, and losing it on a restart is losing a slider position.
    static float scroll = WaterfallView::kDefaultScroll;
    static float lane_height = WaterfallView::kDefaultLaneHeight;

    // The controls take exactly one line, so the plot rectangle is known before they are drawn, and
    // the readout next to them can say what they are set to.
    const float row = ImGui::GetTextLineHeightWithSpacing();
    const float plot_top = origin.y + row + 6.0f;
    const float plot_bottom = origin.y + area_size.y;
    const float plot_height = plot_bottom - plot_top;
    const bool enough = area_size.x >= 88.0f && plot_height >= 24.0f;
    const WaterfallView area(Rect{origin.x, plot_top, area_size.x, plot_height}, scroll,
                             lane_height);
    char span[32];
    std::snprintf(span, sizeof span, "%.1f s on screen", area.visible_seconds());

    ImGui::SetCursorScreenPos(origin);
    // Scroll speed as a bar, not a number dragged sideways. A DragFloat shows digits that change
    // and nothing else, so the setting being made has no shape: this fills from the left in
    // proportion to how fast the past moves up, which is what an eye adjusting it is looking for.
    // Click and drag is the same, and a hand on the keys can also click it and use the arrows,
    // which a DragFloat has no way to do.
    ImGui::SetNextItemWidth(kScrollBarWidth);
    ImGui::SliderFloat("Scroll", &scroll, WaterfallView::kMinScroll, WaterfallView::kMaxScroll,
                       "%.0f px/s");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How fast the past moves up. The number after it is how much music the "
                          "area can show at once.");
    ImGui::SameLine();
    ImGui::DragFloat("Lane height", &lane_height, 0.5f, WaterfallView::kMinLaneHeight,
                     WaterfallView::kMaxLaneHeight, "%.1f px");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "The shortest bar a note is drawn at. A staccato note off a cable is a few "
            "milliseconds, which is a fraction of a pixel at any readable scroll "
            "speed, and a note you heard that the screen refuses to show is a screen "
            "that disagrees with your ear.");

    ImGui::SameLine();
    ImGui::TextUnformatted(span);

    if (!enough) {
        // Same rule as the keyboard: below this the columns are narrower than a pixel and the view
        // says so instead of drawing 88 lanes two pixels wide that no player can follow.
        const char* too_narrow = "too narrow for 88 lanes";
        draw->AddText(ImVec2(origin.x + 4.0f, plot_top + 4.0f), ImGui::GetColorU32(kDim),
                      too_narrow);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, plot_bottom));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        return;
    }

    const double now = view.transport_running ? view.transport_seconds : 0.0;

    // The lanes, and the octave they sit in. Drawn before the notes so a note is always the
    // brightest thing on its column.
    draw->AddRect(ImVec2(origin.x, plot_top), ImVec2(origin.x + area_size.x, plot_bottom),
                  ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.13f)), 0.0f, 0, 1.0f);
    for (int note = KeyboardLayout::kFirstNote; note <= KeyboardLayout::kLastNote; ++note) {
        if (note % 12 != 0)
            continue; // the Cs, which is how a player finds the column they are looking at
        const Rect key = area.keys().rect_for(note);
        draw->AddLine(ImVec2(key.x, plot_top), ImVec2(key.x, plot_bottom),
                      ImGui::GetColorU32(kRuler), 1.0f);
    }

    int outside = 0;
    if (view.transport_running) {
        // The band is the top `fade_band` pixels of the plot: alpha 0 at the area's edge and full
        // at the band's lower edge, so a bar is undimmed until its oldest end reaches the band and
        // gone exactly where the area ends.
        const float fade_band = std::min(kFadePixels, plot_height * 0.5f);
        const float band_bottom = plot_top + fade_band;
        for (int index = 0; index < view.trail_count; ++index) {
            const TrailNote& note = view.trail[static_cast<std::size_t>(index)];
            WaterfallBar bar;
            if (!area.bar_for(note, now, &bar)) {
                // A note the 88 columns cannot place. Counted and printed rather than skipped: a
                // display that quietly drops note 9 is a display nobody can trust about pitch.
                if (!KeyboardLayout::contains(note.note))
                    ++outside;
                continue;
            }
            // A note that has decayed is dimmer than a fresh one (the level term), and a note on
            // its way out is dissolving into the edge rather than blinking at it.
            const ImVec4& hue = kChannelColour[bar.channel & 15u];
            const float base = 0.68f + 0.32f * bar.level;
            if (base <= 0.002f)
                continue;
            if (bar.rect.y >= band_bottom) {
                // Whole of it below the band: one flat colour. The common case, which is worth the
                // branch on its own - a held chord at 320 px/s is a column the height of the plot.
                draw->AddRectFilled(ImVec2(bar.rect.x, bar.rect.y),
                                    ImVec2(bar.rect.right(), bar.rect.bottom()),
                                    ImGui::GetColorU32(ImVec4(hue.x, hue.y, hue.z, base)));
                continue;
            }
            // Part of the bar is inside the band. That part is a gradient to nothing at the area's
            // edge and the part below the band keeps its brightness - not one alpha for the whole
            // bar, which is what used to switch a long note's tail off the moment its head reached
            // the band and made the waterfall look as though it stopped short of the top.
            const float grad_bottom = std::min(bar.rect.bottom(), band_bottom);
            const float at_edge =
                base * std::clamp((grad_bottom - plot_top) / fade_band, 0.0f, 1.0f);
            draw->AddRectFilledMultiColor(ImVec2(bar.rect.x, bar.rect.y),
                                          ImVec2(bar.rect.right(), grad_bottom),
                                          ImGui::GetColorU32(ImVec4(hue.x, hue.y, hue.z, 0.0f)),
                                          ImGui::GetColorU32(ImVec4(hue.x, hue.y, hue.z, 0.0f)),
                                          ImGui::GetColorU32(ImVec4(hue.x, hue.y, hue.z, at_edge)),
                                          ImGui::GetColorU32(ImVec4(hue.x, hue.y, hue.z, at_edge)));
            if (bar.rect.bottom() > band_bottom)
                draw->AddRectFilled(ImVec2(bar.rect.x, band_bottom),
                                    ImVec2(bar.rect.right(), bar.rect.bottom()),
                                    ImGui::GetColorU32(ImVec4(hue.x, hue.y, hue.z, base)));
        }
    }

    // The keyboard line: where a note starts and where it stops being drawn. The playhead
    // everywhere else in this app is a vertical line in one colour at one width (the transport
    // strip says so in its own comment); here the same instant is a horizontal line at the foot of
    // the plot, in that colour and that width, because time runs upwards and the player has to be
    // able to tell where "now" is without being told twice.
    draw->AddLine(ImVec2(origin.x - 3.0f, plot_bottom),
                  ImVec2(origin.x + area_size.x + 3.0f, plot_bottom), ImGui::GetColorU32(kAccent),
                  2.0f);

    if (!view.transport_running) {
        const char* waiting = "no song  \xc2\xb7  play a key to start the clock";
        const float size = ImGui::GetFontSize();
        const float width = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, waiting).x;
        draw->AddText(
            ImGui::GetFont(), size,
            ImVec2(origin.x + (area_size.x - width) * 0.5f, plot_top + (plot_height - size) * 0.5f),
            ImGui::GetColorU32(kDim), waiting);
    } else if (outside > 0) {
        // The instrument is fixed at A0 to C8 (plan 5), so a note outside it has nowhere to be
        // drawn and this says how many - the same honesty the on-screen keyboard shows with its EXT
        // well.
        char ext[32];
        std::snprintf(ext, sizeof ext, "%d outside A0-C8", outside);
        draw->AddText(ImVec2(origin.x + 4.0f, plot_top + 2.0f), ImGui::GetColorU32(kDim), ext);
    }

    // The panel consumed its region. Without this the child window sizes itself to the control row
    // and clips the plot on a short frame - the same trap the transport strip documents.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, plot_bottom));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}

} // namespace viz
} // namespace boreaz::gui
