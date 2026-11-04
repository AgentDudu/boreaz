#pragma once

#include <functional>
#include <string>
#include <vector>

#include "boreaz/engine/engine.h"
#include "boreaz/engine/instrument.h"
#include "boreaz/gui/frame_view.h"

namespace boreaz::gui {

// The mixer: every engine parameter, drawn from the parameter table, and nothing else.
//
// It is a floating window rather than a strip across the top of the keyboard because the keyboard
// and the transport already own that space and the player asked for less clutter there: the panel
// is opened when the sound is being built and closed when the music is being played.
//
// The rows are not written here. `engine::param_table` is the one list of what a parameter is
// called, what it is measured in, and how far it may go, and this walks it - which is why adding a
// parameter is one row there and no change here.
class ParamsPanel {
  public:
    // The instrument is the app's business, not the window's: a sample library is opened with file
    // I/O, and the audio thread must never touch a file. So the panel asks, and the app loads.
    struct Instruments {
        // Load the library at `path`, or go back to the synthesiser when `path` is empty. Returns
        // the instrument to play, or nullptr with a reason in `error`.
        std::function<const boreaz::engine::Instrument*(const std::string& path,
                                                        std::string& error)>
            load;
        // Which instrument the engine actually started playing. The app needs this to free the one
        // before it, which it may only do once nothing is sounding.
        std::function<void(const boreaz::engine::Instrument* instrument)> applied;
    };

    // Draw it, if it is open. Called once per frame with the same frame view the panels get.
    void draw(boreaz::engine::Engine& engine, const FrameView& view,
              const Instruments& instruments);

    // Play this instrument - `nullptr` is the synthesiser - as soon as nothing is sounding. The
    // Load button calls it and so does a test: adopting an instrument rebuilds the voice pool, so
    // the moment it happens is a rule about notes, not a detail of the widget that asked.
    void request(const boreaz::engine::Instrument* instrument);

    // The patch file this panel adopted most recently, empty until it adopts one. The app asks when
    // it closes: the app loaded a patch at startup and has not seen the one the player clicked
    // since.
    const std::string& last_patch() const { return last_patch_; }

    void toggle() { visible_ = !visible_; }
    bool visible() const { return visible_; }

    // The last thing the panel had to say about a load. It is the browser's own line, and the only
    // way to tell a warning from silence - which is a promise, not a detail.
    const std::string& status() const { return status_; }

    // A patch the panel can offer: the file, the word to put on its row, and the sampled instrument
    // its header asks for (empty means the patch says nothing about the instrument, and what is
    // already playing keeps playing).
    struct PatchEntry {
        std::string path;
        std::string name; // what its own header calls it; empty when the file says nothing
        std::string label;
        std::string instrument;
    };

    // Adopt a patch. The parameters go in through the engine's own ramp, and the instrument the
    // patch names follows it. A file that is refused changes nothing at all, including the sound.
    // A browser row calls this; it is public because this suite cannot click, and the thing the
    // criterion asks about - a patch carrying its voice kind - is decided here.
    void adopt_patch(const PatchEntry& patch, boreaz::engine::Engine& engine,
                     const Instruments& instruments);

  private:
    // A requested instrument the engine has not taken yet, and why it is waiting: switching pools
    // while notes are sounding would cut them off, and the plan says the switch may not click.
    void carry_pending(boreaz::engine::Engine& engine, const Instruments& instruments);

    // Read the patches directory. Done only when the list is stale: the panel draws at frame rate,
    // and a directory walk at frame rate is file I/O nobody asked for.
    void scan_patches();

    // Put the library field at `path` and ask for it, the way the Load button does - which is the
    // one place the app is allowed to open a sample library. False means it did not load, and the
    // reason is already in `status_`.
    bool request_instrument(const std::string& path, const Instruments& instruments);

    // Save what the panel is showing, under the name in the field, with the library field written
    // as the patch's instrument.
    void save_patch();

    bool visible_ = false;
    bool synced_ = false; // the first frame takes the engine's parameters rather than defaults
    boreaz::engine::Params params_{};
    // Only part of the table is on screen at once now that the matrix is in it, so the panel can be
    // asked for one parameter instead of scrolled through for one.
    char filter_[48] = "";
    char path_[256] = "";             // the text in the library field
    char patch_name_[96] = "";        // the name a save writes this panel under
    std::vector<PatchEntry> patches_; // the rows the browser is showing
    bool patches_stale_ = true;
    // A path rather than a name: there are two patch folders and a name is not unique between them.
    std::string last_patch_;
    std::string status_; // the last thing the panel has to say about a load
    const boreaz::engine::Instrument* pending_ = nullptr;
    bool pending_set_ = false; // distinguish "the synth is queued" from "nothing is queued"
    bool waiting_on_notes_ = false;
};
} // namespace boreaz::gui
