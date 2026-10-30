// The transport panel: the timeline the instrument plays along, and one line saying what is
// answering.
//
// Read-only, and it only moves when the music does: the window scrolls backwards from the
// transport's position, which sits still until a song is loaded or a note is played, and parks when
// the song ends. There is no song to seek through yet - the MIDI file player arrives with the
// scheduler - so while it runs the playhead stays pinned to the right edge, which shows motion
// without pretending to know how long the song is.
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
#include "boreaz/gui/keyboard_layout.h"

namespace boreaz::gui {
namespace {

// The scrolling window behind the playhead.
constexpr double kWindowSeconds = 8.0;
constexpr int kSecondsPerRulerLine = 1;

// The clock, drawn twice the size of anything else in the window because it is the number read from
// across the room. The strip starts below it and not behind it: a playhead and a ruler line running
// through the digits the player is trying to read makes neither of them readable.
constexpr float kClockHeight = 26.0f;
constexpr float kClockGap = 6.0f;

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
    // sets `transport_running`; until it is true, this draws nothing, which is the honest state.
    const bool rolling = view.transport_running;
    if (rolling) {
        char elapsed[32];
        clock_text(elapsed, sizeof(elapsed), view.transport_seconds);
        // Drawn rather than printed: it is the one number the player reads from across the room, and
        // a draw-list call with an explicit font size stays that size at any content scale.
        const float width = ImGui::CalcTextSize(elapsed).x;
        draw->AddText(ImGui::GetFont(), kClockHeight,
                      ImVec2(origin.x + area.x - width, origin.y),
                      ImGui::GetColorU32(ImVec4(0.95f, 0.96f, 0.95f, 1.0f)), elapsed);
    }

    // The strip fills what it was given: everything under the header - the status line and the
    // clock, whichever of the two reaches lower - except the two meters at its foot. A transport
    // with a margin of unused window around it is a decoration.
    const float clock_floor = rolling ? kClockHeight + kClockGap : 0.0f;
    const float lane_top = origin.y + std::max(row, clock_floor) + 4.0f;
    const float lane_bottom =
        std::max(lane_top, origin.y + area.y - (kMeterHeight * 2.0f + kMeterGap));
    const float lane_width = area.x;
    const float lane_height = lane_bottom - lane_top;

    if (lane_width > 60.0f && lane_height > 8.0f) {
        const ImU32 frame = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.13f));
        const ImU32 tick = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        draw->AddRect(ImVec2(origin.x, lane_top), ImVec2(origin.x + lane_width, lane_bottom), frame,
                      0.0f, 0, 1.0f);

        // The window the strip is showing. While it rolls the window ends at now, so the past
        // scrolls leftwards under a playhead pinned to the right edge. At rest the window runs
        // forward from zero and nothing moves: the same frame, the same gridlines, a playhead at the
        // left where a song would start, and no motion to misread as a transport that is running.
        const double now = rolling ? view.transport_seconds : 0.0;
        const double first = rolling ? now - kWindowSeconds : 0.0;
        const double last = rolling ? now : kWindowSeconds;
        const float pixels_per_second = lane_width / static_cast<float>(kWindowSeconds);
        for (double second = std::ceil(first); second <= last; second += kSecondsPerRulerLine) {
            const float x = origin.x + static_cast<float>((second - first) * pixels_per_second);
            draw->AddLine(ImVec2(x, lane_top), ImVec2(x, lane_bottom), tick, 1.0f);
            // The ticks reach behind the playhead because that is what a ruler does, but before the
            // first note that distance is negative, and a transport printing "-3s" looks like it is
            // running backwards. The numbers start at zero; the ticks do not need to say otherwise.
            if (second < 0.0)
                continue;
            char mark[16];
            std::snprintf(mark, sizeof(mark), "%ds", static_cast<int>(std::lround(second)));
            draw->AddText(ImVec2(x + 3.0f, lane_top + 2.0f), ImGui::GetColorU32(dim), mark);
        }

        // The playhead is pinned to the right edge because the window ends at now. One colour, one
        // width, everywhere it appears - the waterfall will use the identical line, because it is
        // the same object seen from a different view.
        const float head = rolling ? origin.x + lane_width - 1.0f : origin.x;
        const ImU32 accent = ImGui::GetColorU32(ImVec4(0.45f, 0.86f, 0.72f, 1.0f));

        // What has been played, and when: one bar per note, at its own pitch height, from the sample
        // it started on to the sample it stopped on - the playhead, while it still sounds. That is the
        // difference between a clock ticking to itself and an answer to what the hands are doing.
        // Height follows pitch, low at the bottom, so the strip and the keys under it read in the same
        // order; the fill is opaque - the bar *is* the note, and a bar faint enough to miss is a bar
        // that was never drawn as far as a player is concerned - brightening with the note's own
        // level, so a note that has decayed dims on screen the way it does in the room without
        // vanishing while it is still sounding.
        //
        // A note that stops keeps travelling left and leaves slowly. It becomes a bar of its final
        // length, and dropping it at its release would make the strip flicker at the exact moment the
        // ear is still hearing it; instead it scrolls out of the left edge, fading over the last
        // stretch of the strip. That is the waterfall's motion turned through ninety degrees, for the
        // same reason: a thing leaving sight should be leaving gradually.
        if (rolling) {
            const float height =
                std::max(2.0f, lane_height / static_cast<float>(KeyboardLayout::kKeyCount));
            const float span = static_cast<float>(KeyboardLayout::kKeyCount - 1);
            // How much of the strip a note spends fading on its way out. A fifth of the window is
            // over two seconds of travel, which is slow enough to read as leaving and not a glitch.
            constexpr float kFadeFraction = 0.45f;
            for (int index = 0; index < view.trail_count; ++index) {
                const TrailNote& note = view.trail[static_cast<std::size_t>(index)];
                const float start =
                    head - static_cast<float>(now - note.onset_seconds) * pixels_per_second;
                const float stop = note.sounding()
                                       ? head
                                       : head -
                                             static_cast<float>(now - note.end_seconds) *
                                                 pixels_per_second;
                if (stop <= origin.x || start >= head)
                    continue;  // off the strip at one end or the other
                const float left = std::max(start, origin.x);
                const float right = std::min(stop, head);

                // The fade reads the bar's leading edge, so a note still sounding - whose leading
                // edge is the playhead - is never dimmed for having been going since the beginning.
                const float across = (right - origin.x) / lane_width;
                float leave = 1.0f;
                if (across < kFadeFraction) {
                    const float t = across / kFadeFraction;
                    leave = t * t * (3.0f - 2.0f * t);  // smoothstep: the fade has no corner
                }
                if (leave <= 0.002f)
                    continue;

                const float rank = std::clamp(static_cast<float>(note.note) -
                                                  static_cast<float>(KeyboardLayout::kFirstNote),
                                              0.0f, span) / span;
                const float y = lane_bottom - 1.0f - rank * (lane_height - height);
                const float alpha = (0.68f + 0.32f * note.level) * leave;
                draw->AddRectFilled(ImVec2(left, y),
                                    ImVec2(std::max(right, left + 1.0f), y + height),
                                    ImGui::GetColorU32(ImVec4(0.45f, 0.86f, 0.72f, alpha)));
            }
        }

        draw->AddLine(ImVec2(head, lane_top - 3.0f), ImVec2(head, lane_bottom + 3.0f), accent, 2.0f);

        if (!rolling) {
            // Why nothing is moving, in the one place the eye goes looking for motion.
            const char* waiting = "no song  Â·  play a key to start the clock";
            const float size = ImGui::GetFontSize();
            const float width = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, waiting).x;
            draw->AddText(ImGui::GetFont(), size,
                          ImVec2(origin.x + (area.x - width) * 0.5f,
                                 lane_top + (lane_height - size) * 0.5f),
                          ImGui::GetColorU32(dim), waiting);
        }
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
