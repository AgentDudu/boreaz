#include "boreaz/gui/keyboard_view.h"

#include <algorithm>
#include <cstdio>

#include <imgui.h>

#include "boreaz/dsp/tuning.h"

namespace boreaz::gui {
namespace {

// Bone and ebony, and amber for whatever is sounding. A note the sustain pedal is holding on to
// is the same amber one shade down, because "you are holding this" and "the pedal remembers it"
// look identical otherwise, and that difference is the one thing a sustain pedal display has to
// get right.
constexpr ImVec4 kWhite{0.90f, 0.89f, 0.86f, 1.0f};
constexpr ImVec4 kBlack{0.10f, 0.10f, 0.11f, 1.0f};
constexpr ImVec4 kWhiteOutline{0.35f, 0.34f, 0.32f, 1.0f};
constexpr ImVec4 kBlackOutline{0.45f, 0.44f, 0.42f, 1.0f};
constexpr ImVec4 kSounding{0.95f, 0.62f, 0.12f, 1.0f};
constexpr ImVec4 kHover{0.72f, 0.82f, 0.95f, 1.0f};
constexpr ImVec4 kMiddleCMark{0.30f, 0.28f, 0.26f, 1.0f};

ImVec4 mix(const ImVec4& from, const ImVec4& to, float amount) {
    const float t = std::clamp(amount, 0.0f, 1.0f);
    return ImVec4{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                  from.z + (to.z - from.z) * t, 1.0f};
}

ImU32 color(const ImVec4& rgba) {
    return ImGui::GetColorU32(rgba);
}

} // namespace

std::size_t KeyboardView::draw(const FrameView& view, const KeyboardLayout& layout,
                               std::int64_t tick, std::uint8_t channel,
                               boreaz::engine::Event* out, std::size_t capacity) {
    std::size_t produced = 0;
    if (capacity == 0u)
        return 0u;
    ImDrawList* list = ImGui::GetWindowDrawList();

    // Per-key brightness and hold state, from the voices the app copied into the frame view. Two
    // voices on one key - a retrigger while the old one is still dying - take the brighter, which
    // is what the eye expects from a key that is being hit again.
    struct KeyState {
        float level = 0.0f;
        bool held = false;
        bool sounding = false;
    };
    // Indexed by note - kFirstNote, which is the keyboard's own key number: 0 is A0 and 87 is C8.
    KeyState keys[static_cast<std::size_t>(KeyboardLayout::kKeyCount)]{};
    for (int index = 0; index < view.sounding_count; ++index) {
        const Sounding& entry = view.sounding[static_cast<std::size_t>(index)];
        if (!KeyboardLayout::contains(entry.note))
            continue; // EXT: counted in the panel, and drawn nowhere here, because there is no key
        KeyState& key = keys[static_cast<std::size_t>(entry.note - KeyboardLayout::kFirstNote)];
        key.level = key.level > entry.level ? key.level : entry.level;
        key.sounding = true;
        key.held = key.held || entry.held;
    }

    for (int note = KeyboardLayout::kFirstNote; note <= KeyboardLayout::kLastNote; ++note) {
        const bool black = KeyboardLayout::is_black(note);
        const Rect key = layout.rect_for(note);
        const KeyState& state = keys[static_cast<std::size_t>(note - KeyboardLayout::kFirstNote)];
        const bool hovered = note == hovered_note_;

        ImVec4 fill = black ? kBlack : kWhite;
        if (state.sounding) {
            // A note the pedal is holding glows at half the brightness of one being held down, so
            // the keyboard never claims the player is holding something they released.
            const float strength = state.held ? state.level : state.level * 0.5f;
            fill = mix(fill, kSounding, strength);
        } else if (hovered) {
            fill = mix(fill, kHover, 0.25f);
        }

        list->AddRectFilled(ImVec2{key.x, key.y}, ImVec2{key.right(), key.bottom()}, color(fill));
        list->AddRect(ImVec2{key.x, key.y}, ImVec2{key.right(), key.bottom()},
                      color(black ? kBlackOutline : kWhiteOutline), 0.0f, 0, 1.0f);
        if (KeyboardLayout::is_middle_c(note) && !black) {
            // Middle C gets a mark of its own rather than a label the user has to read: it is
            // where every keyboard, every patch and every instruction says "put your left thumb".
            list->AddCircleFilled(ImVec2{key.center_x(), key.bottom() - 9.0f}, 2.5f,
                                  color(kMiddleCMark));
            list->AddText(ImVec2{key.center_x() - 8.0f, key.bottom() - 22.0f},
                          color(kMiddleCMark), "C4");
        }
    }

    // The compass, at the ends, where they cannot be mistaken for anything else.
    if (layout.width() > 260.0f) {
        const Rect first = layout.white_rect(KeyboardLayout::kFirstNote);
        const Rect last = layout.white_rect(KeyboardLayout::kLastNote);
        list->AddText(ImVec2{first.x + 2.0f, first.bottom() - 14.0f}, color(kMiddleCMark), "A0");
        const ImVec2 size = ImGui::CalcTextSize("C8");
        list->AddText(ImVec2{last.right() - size.x - 2.0f, last.bottom() - 14.0f},
                      color(kMiddleCMark), "C8");
    }
    if (view.ext_voices > 0) {
        // Something outside A0-C8 is sounding. It is audible, this strip has no key for it, and a
        // display that silently drops it teaches the player that their ears are wrong.
        char text[48];
        std::snprintf(text, sizeof(text), "%d EXT", view.ext_voices);
        list->AddText(ImVec2{layout.x() + 4.0f, layout.y() + 2.0f},
                      color(ImVec4{0.95f, 0.35f, 0.30f, 1.0f}), text);
    }

    // The click. Aimed anywhere in the strip, at the height of the key bed it was struck: see
    // KeyboardLayout for the geometry and the velocity curve, both of which are tested without a
    // window because the answers cannot depend on a GPU.
    const ImVec2 pointer = ImGui::GetIO().MousePos;
    const bool inside = layout.bounds().contains(pointer.x, pointer.y);
    hovered_note_ = inside ? layout.note_at(pointer.x, pointer.y) : -1;
    if (inside)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (pressed_note_ >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        out[produced++] = boreaz::engine::Event::note_off(
            tick, channel, static_cast<std::uint8_t>(pressed_note_));
        pressed_note_ = -1;
        return produced;
    }
    if (pressed_note_ < 0 && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int note = layout.note_at(pointer.x, pointer.y);
        if (note >= 0) {
            // The note came from the strip, so it is inside A0-C8 by construction and needs no
            // EXT check here; the engine still transposes it like any other key.
            pressed_note_ = note;
            out[produced++] = boreaz::engine::Event::note_on(
                tick, channel, static_cast<std::uint8_t>(note),
                static_cast<std::uint8_t>(layout.velocity_at(pointer.y)));
        }
    }
    return produced;
}

std::size_t KeyboardView::force_release(std::int64_t tick, std::uint8_t channel,
                                        boreaz::engine::Event* out, std::size_t capacity) {
    if (pressed_note_ < 0 || out == nullptr || capacity == 0u)
        return 0u;
    out[0] = boreaz::engine::Event::note_off(tick, channel,
                                             static_cast<std::uint8_t>(pressed_note_));
    pressed_note_ = -1;
    return 1u;
}

} // namespace boreaz::gui
