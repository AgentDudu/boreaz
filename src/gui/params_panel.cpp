// The mixer window. Every widget here is drawn from `engine::param_table`, which is why this file
// has no list of parameters in it: the table says what exists, and a parameter added there appears
// here on the next build with no edit below.
//
// What the panel does to the engine is one call - `set_params` - and what it does to the app is one
// call - `Instruments::load`. It never opens a file and never touches a voice pool.

#include "boreaz/gui/params_panel.h"

#include <imgui.h>

#include <cstdio>
#include <string>

#include "boreaz/engine/param_table.h"
#include "boreaz/gui/keyboard_layout.h"

namespace boreaz::gui {
namespace {

constexpr float kPanelWidth = 340.0f;
constexpr int kMiddleC = boreaz::gui::KeyboardLayout::kMiddleC;
// "C4", "A#4": the note a player hears, next to the number the engine uses. The mixer is where a
// transpose gets checked, and "72" does not check anything.
const char* note_name(int note, char* buffer, std::size_t size) {
    static constexpr const char* kNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                             "F#", "G",  "G#", "A",  "A#", "B"};
    std::snprintf(buffer, size, "%s%d", kNames[((note % 12) + 12) % 12], note / 12 - 1);
    return buffer;
}

} // namespace

void ParamsPanel::request(const boreaz::engine::Instrument* instrument) {
    pending_ = instrument;
    pending_set_ = true;
    status_ = instrument != nullptr ? "library queued" : "synthesiser queued";
}

void ParamsPanel::carry_pending(boreaz::engine::Engine& engine, const Instruments& instruments) {
    if (!pending_set_)
        return;
    // Not while a note is sounding. `set_instrument` rebuilds the voice pool, which is an
    // allocation and a delete, and neither belongs on the audio thread - nor does cutting off a
    // note that was already struck. So the request waits for the tail, and the panel says that is
    // what it is doing.
    if (engine.active_voices() != 0) {
        waiting_on_notes_ = true;
        return;
    }
    engine.set_instrument(pending_);
    if (instruments.applied)
        instruments.applied(pending_);
    status_ = pending_ != nullptr ? "playing the loaded library" : "playing the synthesiser";
    pending_set_ = false;
    pending_ = nullptr;
    waiting_on_notes_ = false;
}

void ParamsPanel::draw(boreaz::engine::Engine& engine, const FrameView& view,
                       const Instruments& instruments) {
    if (!visible_)
        return;

    if (!synced_) {
        // Take what the engine has been told to play rather than assuming defaults: the app may
        // have been given a parameter file on the command line, and a mixer that disagreed with the
        // sound was useless.
        params_ = engine.params();
        synced_ = true;
    }

    ImGui::SetNextWindowSize(ImVec2{kPanelWidth, 0.0f}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mixer", &visible_)) {
        ImGui::End();
        return;
    }

    bool dirty = false;
    std::size_t rows = 0;
    const boreaz::engine::ParamSpec* table = boreaz::engine::param_table(&rows);
    for (std::size_t row = 0; row < rows; ++row) {
        const boreaz::engine::ParamSpec& spec = table[row];
        ImGui::PushID(spec.key);
        // The unit goes next to the number, not in a column header: a slider has to be readable in
        // the same glance as the slider.
        const std::string label = std::string(spec.label) + "##" + spec.key;
        if (spec.real != nullptr) {
            float value = params_.*(spec.real);
            const std::string format = std::string("%0.3f") + spec.unit;
            if (ImGui::SliderFloat(label.c_str(), &value, spec.minimum, spec.maximum,
                                   format.c_str())) {
                boreaz::engine::param_write(spec, value, params_);
                dirty = true;
            }
        } else {
            int value = params_.*(spec.integer);
            const std::string format = std::string("%d") + spec.unit;
            if (ImGui::SliderInt(label.c_str(), &value, static_cast<int>(spec.minimum),
                                 static_cast<int>(spec.maximum), format.c_str())) {
                boreaz::engine::param_write(spec, static_cast<float>(value), params_);
                dirty = true;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%g to %g%s, default %g%s", spec.key, spec.minimum, spec.maximum,
                              spec.unit, spec.default_value, spec.unit);
        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::Button("Defaults")) {
        params_ = boreaz::engine::Params{};
        dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every slider back to where the instrument starts.");
    if (dirty)
        engine.set_params(params_); // the engine's ramp is what keeps a slider from clicking

    // Where the keyboard is. A transpose of 12 is a number; "middle C plays C5" is the check that
    // the number means what the player meant.
    char from[16];
    char to[16];
    ImGui::Text("%s -> %s  (%d sounding)", note_name(kMiddleC, from, sizeof from),
                note_name(engine.sounding_note(kMiddleC), to, sizeof to), view.sounding_count);

    ImGui::Separator();
    ImGui::TextUnformatted("Instrument");
    ImGui::SetNextItemWidth(kPanelWidth - 130.0f);
    ImGui::InputText("##library", path_, sizeof path_);
    const bool can_load = static_cast<bool>(instruments.load);
    ImGui::SameLine();
    if (ImGui::Button("Load") && can_load) {
        std::string reason;
        const boreaz::engine::Instrument* found =
            instruments.load(path_, reason); // file I/O, on the UI thread, never the audio one
        if (found != nullptr) {
            request(found);
        } else if (path_[0] == '\0') {
            request(nullptr); // an empty path means the synthesiser, which is not an error
        } else {
            status_ = reason;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Synth") && can_load) {
        std::string reason;
        instruments.load("", reason); // the app releases the library here; the synth needs nothing
        request(nullptr);
    }
    carry_pending(engine, instruments); // lands on the first frame the pool has gone quiet
    if (waiting_on_notes_)
        ImGui::Text("waiting for %d note(s) to stop", engine.active_voices());
    else if (!status_.empty())
        ImGui::TextWrapped("%s", status_.c_str());

    ImGui::End();
}

} // namespace boreaz::gui
