// The transport panel: where the instrument is, and how it is doing.
//
// Read-only. There is no song to seek through yet - the MIDI file player arrives with the
// scheduler - so this strip reports where the audio thread has got to and what it cost, which is
// also the only way to see the frame loop and the audio clock from outside a profiler.
//
// It draws into whatever rectangle the app gave it and reads nothing but the FrameView. That is the
// whole contract between a panel and the audio path (plan 3): no engine pointer, no device, no way
// to delay a block.

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "boreaz/gui/panel.h"

namespace boreaz::gui {
namespace {

// The scrolling window behind the playhead. The position has no end to measure against until there
// is a file loaded, so the ruler runs backwards from now: the playhead stays at the right-hand edge
// and the past scrolls past it, which shows motion without pretending to know how long the song is.
constexpr double kWindowSeconds = 8.0;
constexpr int kSecondsPerRulerLine = 1;

void clock_text(char* buffer, std::size_t size, double seconds) {
    const double whole = std::max(0.0, seconds);
    const int minutes = static_cast<int>(whole / 60.0);
    const int remainder = static_cast<int>(std::fmod(whole, 60.0));
    const int millis = static_cast<int>(std::fmod(whole, 1.0) * 1000.0);
    std::snprintf(buffer, size, "%d:%02d.%03d", minutes, remainder, millis);
}

// One label/value line, the value in the colour the readouts share.
void line(const char* label, const std::string& value, bool bad = false) {
    const float start = ImGui::GetCursorPosX();
    const float room = ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetCursorPosX(start + room - ImGui::CalcTextSize(value.c_str()).x);
    ImGui::PushStyleColor(ImGuiCol_Text,
                          bad ? ImVec4(0.93f, 0.45f, 0.36f, 1.0f) : ImVec4(0.78f, 0.85f, 0.82f, 1.0f));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
}

void bar(const char* label, float fraction, const ImVec4& colour) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 5.0f);
    const ImVec2 where = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(where, ImVec2(where.x + size.x, where.y + size.y),
                        ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.45f)));
    const float filled = std::clamp(fraction, 0.0f, 1.0f) * size.x;
    draw->AddRectFilled(where, ImVec2(where.x + filled, where.y + size.y),
                        ImGui::GetColorU32(colour));
    ImGui::Dummy(ImVec2(size.x, size.y + 3.0f));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 1.0f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 11.0f);
    ImGui::TextUnformatted(label);
}

} // namespace

namespace viz {

void transport(const FrameView& view) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 area = ImGui::GetContentRegionAvail();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // The readout column takes whatever the ruler does not need, and never so much that the ruler
    // loses its own legibility - below a couple of hundred pixels of lane the second gridlines
    // overlap and the strip says nothing.
    const float readout_width = std::min(330.0f, std::max(0.0f, area.x * 0.40f));
    const float lane_width = area.x - readout_width - 28.0f;
    const float lane_height = std::min(58.0f, area.y);

    char elapsed[32];
    clock_text(elapsed, sizeof(elapsed), view.position_seconds());
    // The clock is drawn rather than printed: it is the one number the player reads from across the
    // room, and ImGui's own text size is the size of a label. A draw-list call with an explicit font
    // size keeps it that way at any content scale, where a scaled ImGui::Text would fight the DPI.
    const float clock_height = 26.0f;
    draw->AddText(ImGui::GetFont(), clock_height, origin,
                  ImGui::GetColorU32(ImVec4(0.95f, 0.96f, 0.95f, 1.0f)), elapsed);

    const float lane_top = origin.y + clock_height + 12.0f;
    if (lane_width > 160.0f) {
        const ImU32 frame = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.13f));
        const ImU32 tick = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        draw->AddRect(ImVec2(origin.x, lane_top),
                      ImVec2(origin.x + lane_width, lane_top + lane_height), frame, 0.0f,
                      0, 1.0f);

        // Gridlines every second of the window, labelled at the top. The labels move with the
        // window, so the scroll is readable as time and not as a bar animating for no reason.
        const double now = view.position_seconds();
        const float pixels_per_second = lane_width / static_cast<float>(kWindowSeconds);
        for (double second = std::ceil(now - kWindowSeconds); second <= now;
             second += kSecondsPerRulerLine) {
            const float x = origin.x +
                            static_cast<float>((second - (now - kWindowSeconds)) * pixels_per_second);
            draw->AddLine(ImVec2(x, lane_top), ImVec2(x, lane_top + lane_height), tick, 1.0f);
            char mark[16];
            std::snprintf(mark, sizeof(mark), "%ds", static_cast<int>(std::lround(second)) % 60);
            draw->AddText(ImVec2(x + 3.0f, lane_top + 2.0f),
                          ImGui::GetColorU32(ImVec4(0.62f, 0.66f, 0.62f, 1.0f)), mark);
        }

        // The playhead is pinned to the right edge because the window ends at now. One colour, one
        // width, everywhere it appears - the waterfall will use the identical line, because it is
        // the same object seen in a different view.
        const float head = origin.x + lane_width - 1.0f;
        const ImU32 accent = ImGui::GetColorU32(ImVec4(0.45f, 0.86f, 0.72f, 1.0f));
        draw->AddLine(ImVec2(head, lane_top - 3.0f), ImVec2(head, lane_top + lane_height + 3.0f),
                      accent, 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, lane_top + lane_height + 4.0f));
        ImGui::TextColored(ImVec4(0.62f, 0.66f, 0.62f, 1.0f),
                           "last %.0f s 路 %lld samples 路 %.0f Hz", kWindowSeconds,
                           static_cast<long long>(view.sample_position), view.device.sample_rate);
    }

    // The readout. Every number here is one the audio thread publishes as a relaxed scalar; none of
    // them is read from the engine on this thread, which is what keeps a panel from being able to
    // stall a block.
    const float readout_x = origin.x + lane_width + 28.0f;
    ImGui::SetCursorScreenPos(ImVec2(readout_x, origin.y));
    if (lane_width <= 160.0f)
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y));

    // What the instrument is, in three lines, and then quiet. Every number below is still
    // published by the audio thread and still printed in full when the app exits; what a panel
    // during a performance has to earn is the right to be looked at, and "steals 0" in the same
    // weight as "cpu 47 %" teaches a player to read none of them.
    line("output", view.device.open
                       ? view.device.name + "  \xc2\xb7  " +
                             std::to_string(static_cast<int>(view.device.sample_rate)) + " Hz" +
                             "  \xc2\xb7  " + std::to_string(view.device.callback_frames) + " fr"
                       : "not started");
    line("load", std::to_string(static_cast<int>(view.device.cpu_fraction * 100.0)) + " %" +
                     "  \xc2\xb7  " + std::to_string(view.active_voices) + " / " +
                     std::to_string(view.pool_size) + " voices");
    bar("cpu", static_cast<float>(view.device.cpu_fraction), ImVec4(0.45f, 0.78f, 0.55f, 1.0f));
    line("midi", view.midi.open ? view.midi.name + "  \xc2\xb7  " +
                                      std::to_string(view.midi.events) + " messages"
                                : "no keyboard");

    // A counter that reads zero is not information, so the trouble line appears the moment any of
    // them stops being zero - in the colour that means look here, next to the words that say what
    // broke. Nothing is hidden from anyone: the exit line in the terminal keeps every one of them.
    std::string trouble;
    const auto plus = [&](const char* what, long long count) {
        if (count <= 0)
            return;
        if (!trouble.empty())
            trouble += "  \xc2\xb7  ";
        trouble += what;
        trouble += " " + std::to_string(count);
    };
    plus("late", static_cast<long long>(view.device.late_callbacks));
    plus("restarts", static_cast<long long>(view.device.restarts));
    plus("steals", static_cast<long long>(view.steals));
    plus("dropped", static_cast<long long>(view.dropped_events));
    plus("midi lost", static_cast<long long>(view.midi.losses));
    plus("off-key notes", static_cast<long long>(view.ext_voices));
    if (!trouble.empty())
        line("trouble", trouble, true);
    else if (view.sustain)
        line("sustain", "pedal down");

    // The panel consumed its region; without this the child window would size itself to the text
    // column and clip the ruler on a short frame.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, std::max(origin.y + area.y,
                                                        lane_top + lane_height + 20.0f)));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));
}

} // namespace viz

} // namespace boreaz::gui
