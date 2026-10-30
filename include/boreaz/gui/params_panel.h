#pragma once

#include <functional>
#include <string>

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

    void toggle() { visible_ = !visible_; }
    bool visible() const { return visible_; }

  private:
    // A requested instrument the engine has not taken yet, and why it is waiting: switching pools
    // while notes are sounding would cut them off, and the plan says the switch may not click.
    void carry_pending(boreaz::engine::Engine& engine, const Instruments& instruments);

    bool visible_ = false;
    bool synced_ = false; // the first frame takes the engine's parameters rather than defaults
    boreaz::engine::Params params_{};
    char path_[256] = ""; // the text in the library field
    std::string status_;  // the last thing the panel has to say about a load
    const boreaz::engine::Instrument* pending_ = nullptr;
    bool pending_set_ = false; // distinguish "the synth is queued" from "nothing is queued"
    bool waiting_on_notes_ = false;
};
} // namespace boreaz::gui
