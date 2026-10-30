#pragma once

#include <functional>
#include <memory>
#include <string>

#include "boreaz/engine/engine.h"
#include "boreaz/gui/frame_view.h"
#include "boreaz/gui/keyboard_view.h"
#include "boreaz/gui/note_trail.h"
#include "boreaz/gui/panel.h"
#include "boreaz/gui/transport_clock.h"

namespace boreaz::gui {

// The window: GLFW for the surface, Direct3D 11 for the pixels, Dear ImGui for the widgets.
// Nothing else in the program knows any of that. The app hands over the engine and a function
// that reports device and port numbers; the window draws a frame and pushes whatever the mouse
// did into the engine's event ring, which is the same ring a MIDI cable writes to.
//
// The audio thread is never waited for, never locked against, and never told to hurry up: this
// class reads the telemetry scalars the engine says the display may read, and a frame that shows
// one frame-old numbers is a frame that is 16 ms late, which is what a window already is.
class AppWindow {
  public:
    // The surface's state. Public because GLFW calls its callbacks as plain C function pointers,
    // and the resize callback reaches the swap chain through a pointer we set on the window; keeping
    // the type private would mean a member function trampoline for no benefit.
    struct Impl;

    // Fills in the device and port numbers the panels show, once per frame, on the UI thread. A
    // callback rather than the objects themselves because the window does not know what an audio
    // device is, and that is what lets it build and link without one.
    using StatsFn = std::function<void(DeviceStats& device, MidiStats& midi)>;

    struct Options {
        int width = 1280;
        int height = 720;
        std::string title = "Boreaz";
        std::uint8_t channel = 0; // what the on-screen keys play on
    };

    AppWindow();
    ~AppWindow(); // out of line: Impl is incomplete in this header
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    // A window on screen and a Direct3D device ready to draw. `error` says which of the many
    // things went wrong when it is false, because "it did not open" is not an error message.
    bool open(const Options& options, std::string& error);

    // One frame: pump the window system, draw, present. Returns false when the player closed it
    // or the device went away, which is the app's cue to stop the audio and exit cleanly.
    bool frame(boreaz::engine::Engine& engine, const StatsFn& stats);

    void close();

    double frames() const { return frames_; }
    double seconds_elapsed() const;
    PanelKind panel_kind() const { return panel_; }

  private:
    bool create_surface(std::string& error);
    void destroy_surface();
    void draw(boreaz::engine::Engine& engine, const StatsFn& stats);

    std::unique_ptr<Impl> impl_;
    Options options_;
    KeyboardView keyboard_;
    KeyboardLayout layout_;
    PanelKind panel_ = PanelKind::kTransport;
    TransportClock transport_;
    // The notes the strip scrolls. It lives here and not in a panel because it remembers things
    // between frames, and a panel that owned that memory would lose the past whenever the player
    // switched views - and would be a free function with hidden state, which cannot be tested.
    NoteTrail trail_;
    double frames_ = 0.0;
    bool was_focused_ = true;
};

} // namespace boreaz::gui
