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

#include "boreaz/gui/panel.h"
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

// How much of the top of the area a note spends fading on its way out. The strip's own fade band is
// the same idea turned through ninety degrees, for the same reason: a thing leaving sight should be
// leaving gradually and not blinking out at an edge.
constexpr float kFadeFraction = 0.30f;

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
    ImGui::DragFloat("Scroll", &scroll, 8.0f, WaterfallView::kMinScroll, WaterfallView::kMaxScroll,
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
        const float fade_band = plot_height * kFadeFraction;
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
            // The fill is opaque at the line and fades towards the top, so a note that has decayed
            // is dimmer than a fresh one without vanishing while it still sounds, and one on its
            // way out is leaving rather than blinking.
            const float up = plot_bottom - bar.rect.bottom();
            float leave = 1.0f;
            if (up > plot_height - fade_band) {
                const float t = (plot_bottom - bar.rect.y) / fade_band;
                leave = 1.0f - t;
                leave = std::clamp(leave, 0.0f, 1.0f);
                leave = leave * leave * (3.0f - 2.0f * leave); // smoothstep: the fade has no corner
            }
            const ImVec4& hue = kChannelColour[bar.channel & 15u];
            const float alpha = (0.68f + 0.32f * bar.level) * leave;
            if (alpha <= 0.002f)
                continue;
            draw->AddRectFilled(ImVec2(bar.rect.x, bar.rect.y),
                                ImVec2(bar.rect.right(), bar.rect.bottom()),
                                ImGui::GetColorU32(ImVec4(hue.x, hue.y, hue.z, alpha)));
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
