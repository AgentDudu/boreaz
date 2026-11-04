// The mixer window. Every widget here is drawn from `engine::param_table`, which is why this file
// has no list of parameters in it: the table says what exists, and a parameter added there appears
// here on the next build with no edit below.
//
// What the panel does to the engine is one call - `set_params` - and what it does to the app is one
// call - `Instruments::load`. It reads and writes patch files, which are a few hundred bytes of
// text and belong on the UI thread; it never opens a sample library, because that is a hundred and
// fifty megabytes of file I/O and the app owns it, and it never touches a voice pool.

#include "boreaz/gui/params_panel.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "boreaz/config/params_file.h"
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

// Does this row answer to what has been typed? Both the file name and the word next to the slider,
// because a person searches for "cutoff" and a patch file searches for "filter_". ASCII folding
// only: the keys are ASCII and a label that needed more would be a label worth reading, not
// finding.
bool answers_to(const boreaz::engine::ParamSpec& spec, std::string_view needle) {
    if (needle.empty())
        return true;
    for (std::string_view hay : {std::string_view(spec.key), std::string_view(spec.label)}) {
        auto fold = [](char c) {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        auto hit = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                               [&](char a, char b) { return fold(a) == fold(b); });
        if (hit != hay.end())
            return true;
    }
    return false;
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

bool ParamsPanel::request_instrument(const std::string& path, const Instruments& instruments) {
    // The field shows what the panel asked for, so the next Save writes the same path back into the
    // patch's header and the two halves of a preset travel together.
    std::snprintf(path_, sizeof path_, "%s", path.c_str());
    std::string reason;
    const boreaz::engine::Instrument* found = instruments.load(path, reason);
    if (found != nullptr) {
        request(found);
        return true;
    }
    if (path.empty()) {
        request(nullptr); // an empty path means the synthesiser, which is not an error
        return true;
    }
    status_ = reason;
    return false;
}

void ParamsPanel::scan_patches() {
    patches_.clear();
    patches_stale_ = false;

    std::error_code ignored;
    const std::filesystem::path dir{boreaz::config::patches_dir()};
    if (!std::filesystem::is_directory(dir, ignored))
        return;

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ignored)) {
        if (entry.is_regular_file(ignored) && entry.path().extension() == ".txt")
            files.push_back(entry.path());
    }
    // Sorted by file name, because the list has to be the same order every time it opens: rows that
    // move between frames are a thing you cannot click on.
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        PatchEntry patch;
        patch.path = file.string();
        std::string reason;
        if (!boreaz::config::read_patch_meta(patch.path, patch.name, patch.instrument, reason))
            patch.name = file.filename().string(); // a file that will not open still gets a row
        patch.label = patch.name.empty() ? file.filename().string() : patch.name;
        patches_.push_back(std::move(patch));
    }
}

void ParamsPanel::save_patch() {
    std::string name{patch_name_};
    const auto not_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) == 0; };
    name.erase(name.begin(), std::find_if(name.begin(), name.end(), not_space));
    name.erase(std::find_if(name.rbegin(), name.rend(), not_space).base(), name.end());

    if (name.empty()) {
        status_ = "a patch needs a name before it can be saved";
        return;
    }
    // The name becomes a file name, and a name is text somebody typed into a box. A dot or a slash
    // here is a save that lands outside the patches folder, or one that cannot be read back.
    if (name.find_first_of("/\\:.") != std::string::npos) {
        status_ = "a patch name cannot contain a dot, a slash, or a colon";
        return;
    }

    const std::filesystem::path dir{boreaz::config::patches_dir()};
    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);
    const std::filesystem::path file = dir / (name + ".txt");
    std::string reason;
    if (boreaz::config::save_params_file(file.string(), params_, name, path_, reason)) {
        status_ = "wrote " + file.string();
        patches_stale_ = true; // the row for it appears without a restart
    } else {
        status_ = reason;
    }
}

void ParamsPanel::adopt_patch(const PatchEntry& patch, boreaz::engine::Engine& engine,
                              const Instruments& instruments) {
    // Into a copy of what is playing. The parser refuses a bad key or a bad number for the whole
    // file, and a panel that had already taken half of it would be playing a sound nobody wrote.
    boreaz::engine::Params loaded = engine.params();
    std::string reason;
    if (!boreaz::config::load_params_file(patch.path, loaded, reason)) {
        // The decision this panel makes: the file is refused, the reason is printed, and the sound
        // that was playing keeps playing. A half-loaded patch is worse than one that did not load.
        status_ = "not loaded - " + reason;
        return;
    }
    params_ = loaded;
    engine.set_params(params_); // the engine's ramp is what makes a patch change without a click

    // The instrument the patch names comes with it. One that names nothing keeps the library that
    // is already playing rather than dropping to the synthesiser, and one whose library is missing
    // says so in the panel's own line - which is a warning, not silence.
    if (!patch.instrument.empty() && instruments.load != nullptr &&
        !request_instrument(patch.instrument, instruments))
        return;
    status_ = "loaded " + patch.label;
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
    ImGui::SetNextItemWidth(kPanelWidth - 130.0f);
    ImGui::InputTextWithHint("##find", "find a parameter", filter_, sizeof filter_);
    const std::string_view needle(filter_[0] ? filter_ : "");
    std::size_t shown = 0;
    const boreaz::engine::ParamSpec* table = boreaz::engine::param_table(&rows);
    for (std::size_t row = 0; row < rows; ++row) {
        const boreaz::engine::ParamSpec& spec = table[row];
        if (!answers_to(spec, needle))
            continue;
        ++shown;
        ImGui::PushID(spec.key);
        // The unit goes next to the number, not in a column header: a slider has to be readable in
        // the same glance as the slider.
        const std::string label = std::string(spec.label) + "##" + spec.key;
        // Both branches read and write through the table: a row that lives in an `std::array` has
        // no member pointer of its own, and the panel is not the place to guess which array a row
        // is in.
        std::size_t choices = 0;
        const char* const* words = boreaz::engine::param_choices(spec.array_kind, &choices);
        if (words != nullptr) {
            // A cable ends in a list, not on a scale: six whole numbers on a slider is a widget
            // where "M1 source = 2" has to be remembered rather than read.
            int value = static_cast<int>(boreaz::engine::param_read(spec, params_));
            ImGui::SetNextItemWidth(kPanelWidth - 130.0f);
            if (ImGui::Combo(label.c_str(), &value, words, static_cast<int>(choices))) {
                boreaz::engine::param_write(spec, static_cast<float>(value), params_);
                dirty = true;
            }
        } else if (!boreaz::engine::param_is_integer(spec)) {
            float value = boreaz::engine::param_read(spec, params_);
            const std::string format = std::string("%0.3f") + spec.unit;
            if (ImGui::SliderFloat(label.c_str(), &value, spec.minimum, spec.maximum,
                                   format.c_str())) {
                boreaz::engine::param_write(spec, value, params_);
                dirty = true;
            }
        } else if (spec.minimum == 0.0f && spec.maximum == 1.0f) {
            // A whole number that can only be 0 or 1 is a switch, and drawing it as a slider asks
            // the player to drag a thing that has two positions.
            bool on = boreaz::engine::param_read(spec, params_) != 0.0f;
            if (ImGui::Checkbox(label.c_str(), &on)) {
                boreaz::engine::param_write(spec, on ? 1.0f : 0.0f, params_);
                dirty = true;
            }
        } else {
            int value = static_cast<int>(boreaz::engine::param_read(spec, params_));
            const std::string format = std::string("%d") + spec.unit;
            if (ImGui::SliderInt(label.c_str(), &value, static_cast<int>(spec.minimum),
                                 static_cast<int>(spec.maximum), format.c_str())) {
                boreaz::engine::param_write(spec, static_cast<float>(value), params_);
                dirty = true;
            }
        }
        if (ImGui::IsItemHovered()) {
            // A list row has no range worth printing; the number it stands for is the one a patch
            // file writes, which is what makes the list and the `.ini` the same cable.
            const int index = static_cast<int>(boreaz::engine::param_read(spec, params_));
            if (words != nullptr && index >= 0 && index < static_cast<int>(choices))
                ImGui::SetTooltip("%s\n%s = %d of %d", spec.key, words[index], index,
                                  static_cast<int>(choices) - 1);
            else
                ImGui::SetTooltip("%s\n%g to %g%s, default %g%s", spec.key, spec.minimum,
                                  spec.maximum, spec.unit, spec.default_value, spec.unit);
        }
        ImGui::PopID();
    }
    if (!needle.empty())
        ImGui::Text("%d of %d", static_cast<int>(shown), static_cast<int>(rows));

    ImGui::Separator();
    if (ImGui::Button("Defaults")) {
        params_ = boreaz::engine::Params{};
        dirty = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every slider back to where the instrument starts.");
    if (dirty)
        engine.set_params(params_); // the engine's ramp is what keeps a slider from clicking

    // A patch is this panel written to a file, and the file is the one `--params` reads, so the
    // browser is a directory listing rather than a second store of settings nobody asked for.
    ImGui::Separator();
    ImGui::TextUnformatted("Patch");
    ImGui::SetNextItemWidth(kPanelWidth - 130.0f);
    ImGui::InputTextWithHint("##patchname", "name for a new patch", patch_name_,
                             sizeof patch_name_);
    ImGui::SameLine();
    if (ImGui::Button("Save"))
        save_patch();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Every parameter, plus the library path above, into patches/<name>.txt.\n"
                          "The file loads with --params and with this list.");
    if (patches_stale_)
        scan_patches();
    for (std::size_t row = 0; row < patches_.size(); ++row) {
        ImGui::PushID(static_cast<int>(row));
        if (ImGui::Button(patches_[row].label.c_str()))
            adopt_patch(patches_[row], engine, instruments);
        if (ImGui::IsItemHovered()) {
            const std::string tip =
                patches_[row].instrument.empty()
                    ? patches_[row].path + "\nkeeps the instrument that is playing"
                    : patches_[row].path + "\n" + patches_[row].instrument;
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::PopID();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan"))
        patches_stale_ = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Read the patches folder again - a file dropped in from Explorer appears.");

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
    if (ImGui::Button("Load") && can_load)
        request_instrument(path_, instruments); // file I/O, on the UI thread, never the audio one
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
