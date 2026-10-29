// The transport panel: the timeline the instrument plays along, and one line saying what is
// answering.
//
// Read-only. There is no song to seek through yet - the MIDI file player arrives with the
// scheduler - so the window scrolls backwards from now and the playhead stays pinned to the right
// edge, which shows motion without pretending to know how long the song is.
//
// It draws into whatever rectangle the app gave it, fills it, and reads nothing but the FrameView.
// That is the whole contract between a panel and the audio path (plan 3): no engine pointer, no
// device, no way to delay a block. The waterfall and the piano roll are going to hold the same
// rectangle and the same contract, which is why this file draws in the strip's own coordinates and
// never assumes a size.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "boreaz/gui/panel.h"

namespace boreaz::gui {
namespace {

// The scrolling window behind the playhead.
constexpr double kWindowSeconds = 8.0;
constexpr int kSecondsPerRulerLine = 1;

// The two meters at the foot of the strip: audio CPU and how hard the master is leaning on the soft
// clipper. Bars with no labels and no numbers, because a meter that needs a number under it is a
// table, and this is the part of the window a player glances at while both hands are busy.
constexpr float kMeterHeight = 4.0f;
constexpr float kMeterGap = 3.0f;

void meter(ImDrawList* draw, float x, float y, float width, float fraction, const ImVec4& colour) {
    const ImVec2 from(x, y);
    const ImVec2 to(x + width, y + kMeterHeight);
    draw->AddRectFilled(from, to, ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.45f)));
    const float filled = std::clamp(fraction, 0.0f, 1.0f) * width;
    if (filled > 0.0f)
        draw->AddRectFilled(from, ImVec2(x + filled, to.y), ImGui::GetColorU32(colour));
}

void clock_text(char* buffer, std::size_t size, double seconds) {
    const double whole = std::max(0.0, seconds);
    const int minutes = static_cast<int>(whole / 60.0);
    const int remainder = static_cast<int>(std::fmod(whole, 60.0));
    const int millis = static_cast<int>(std::fmod(whole, 1.0) * 1000.0);
    std::snprintf(buffer, size, "%d:%02d.%03d", minutes, remainder, millis);
}

} // namespace

namespace viz {

void transport(const FrameView& view) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 dim(0.62f, 0.66f, 0.62f, 1.0f);

    // One line, top-left, and the only text in the panel: what is answering, at what rate, at what
    // period, and what is on the cable. The counters that used to sit under it (late callbacks,
    // restarts, steals, dropped events, MIDI losses) are still published every frame and still
    // printed in full when the app exits, which is where a number that reads zero belongs.
    std::string status = view.device.open
                             ? view.device.name + "  \xc2\xb7  " +
                                   std::to_string(static_cast<int>(view.device.sample_rate)) + " Hz" +
                                   "  \xc2\xb7  " + std::to_string(view.device.callback_frames) + " fr"
                             : "no output";
    status += view.midi.open
                  ? "  \xc2\xb7  " + view.midi.name + ", " + std::to_string(view.midi.events) +
                        " messages"
                  : "  \xc2\xb7  no keyboard";

    const float row = ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorScreenPos(origin);
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::TextUnformatted(status.c_str());
    ImGui::PopStyleColor();

    // The clock is the song's position, so it appears when a song does. The audio thread has been
    // counting since the device opened whether or not anything is going to play, and a number in
    // the middle of an empty screen reads like a transport that is about to move. The file player
    // sets `song_loaded`; until it exists, this draws nothing, which is the honest state.
    if (view.song_loaded) {
        char elapsed[32];
        clock_text(elapsed, sizeof(elapsed), view.position_seconds());
        // Drawn rather than printed: it is the one number the player reads from across the room, and
        // a draw-list call with an explicit font size stays that size at any content scale.
        const float clock_height = 26.0f;
        const float width = ImGui::CalcTextSize(elapsed).x;
        draw->AddText(ImGui::GetFont(), clock_height,
                      ImVec2(origin.x + area.x - width, origin.y),
                      ImGui::GetColorU32(ImVec4(0.95f, 0.96f, 0.95f, 1.0f)), elapsed);
    }

    // The strip fills what it was given: everything under the status line except the two meters at
    // its foot. A transport with a margin of unused window around it is a decoration.
    const float lane_top = origin.y + row + 4.0f;
    const float lane_bottom =
        std::max(lane_top, origin.y + area.y - (kMeterHeight * 2.0f + kMeterGap));
    const float lane_width = area.x;
    const float lane_height = lane_bottom - lane_top;

    if (lane_width > 60.0f && lane_height > 8.0f) {
        const ImU32 frame = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.13f));
        const ImU32 tick = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        draw->AddRect(ImVec2(origin.x, lane_top), ImVec2(origin.x + lane_width, lane_bottom), frame,
                      0.0f, 0, 1.0f);

        // Gridlines every second of the window, labelled along the top, moving with the scroll, so
        // what scrolls past reads as time and not as a bar animating for no reason.
        const double now = view.position_seconds();
        const float pixels_per_second = lane_width / static_cast<float>(kWindowSeconds);
        for (double second = std::ceil(now - kWindowSeconds); second <= now;
             second += kSecondsPerRulerLine) {
            const float x = origin.x +
                            static_cast<float>((second - (now - kWindowSeconds)) * pixels_per_second);
            draw->AddLine(ImVec2(x, lane_top), ImVec2(x, lane_bottom), tick, 1.0f);
            char mark[16];
            std::snprintf(mark, sizeof(mark), "%ds", static_cast<int>(std::lround(second)) % 60);
            draw->AddText(ImVec2(x + 3.0f, lane_top + 2.0f), ImGui::GetColorU32(dim), mark);
        }

        // The playhead is pinned to the right edge because the window ends at now. One colour, one
        // width, everywhere it appears - the waterfall will use the identical line, because it is
        // the same object seen from a different view.
        const float head = origin.x + lane_width - 1.0f;
        const ImU32 accent = ImGui::GetColorU32(ImVec4(0.45f, 0.86f, 0.72f, 1.0f));
        draw->AddLine(ImVec2(head, lane_top - 3.0f), ImVec2(head, lane_bottom + 3.0f), accent, 2.0f);
    }

    // Meters across the foot of the strip, so the panel's bottom edge is the load and the top edge
    // of the keyboard is the instrument. Left is the audio thread's share of a core; right is the
    // master before the limiter, so it is the clip light and it reaches full width at full scale.
    const float meter_y = lane_bottom + kMeterGap;
    const float half = (area.x - 8.0f) * 0.5f;
    meter(draw, origin.x, meter_y, half, static_cast<float>(view.device.cpu_fraction),
          ImVec4(0.45f, 0.78f, 0.55f, 1.0f));
    meter(draw, origin.x + half + 8.0f, meter_y, half, view.drive_peak,
          ImVec4(0.95f, 0.62f, 0.12f, 1.0f));

    // The panel consumed its region. Without this the child window sizes itself to the text line and
    // clips the strip on a short frame.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + area.y));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}

} // namespace viz

} // namespace boreaz::gui
