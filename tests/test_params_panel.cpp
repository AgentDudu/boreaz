// The mixer panel's one rule that could break the audio path: an instrument the player asked for is
// not installed while notes are sounding.
//
// Everything else the panel does is a slider writing into `Params`, which is tested through the
// table in test_live_params.cpp - the panel reads that same table, so a parameter in the table is a
// parameter the panel reaches. What is tested here is the queue in front of
// `Engine::set_instrument`, because that call rebuilds the voice pool: allocations, deletes, and
// the end of every note that was still ringing.

#include <string>

#include <doctest/doctest.h>

#include <imgui.h>

#include "boreaz/engine/engine.h"
#include "boreaz/engine/instrument.h"
#include "boreaz/gui/frame_view.h"
#include "boreaz/gui/params_panel.h"
#include "engine_fixture.h"

using boreaz::engine::Engine;
using boreaz::engine::Event;
using boreaz::engine::Instrument;
using boreaz::engine::VoiceSample;
using boreaz::gui::FrameView;
using boreaz::gui::ParamsPanel;

namespace {

// An instrument with nothing in it: legal, silent, and a pointer a test can recognise.
class FakeInstrument final : public Instrument {
  public:
    const VoiceSample* find(int, int) const override { return nullptr; }
    bool empty() const override { return true; }
    std::size_t resident_bytes() const override { return 0; }
    std::size_t refused_regions() const override { return 0; }
};

// Dear ImGui with no renderer: a frame is drawn into a draw list nobody looks at. It needs a
// context and a display size, and nothing else, which is the only reason this queue is testable at
// all. The context is a process-wide global, so there is exactly one, created here and shared by
// every case.
struct Ui {
    Ui() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2{1280.0f, 720.0f};
        io.DeltaTime = 1.0f / 60.0f;
        // A renderer normally does this. Without a font every widget measurement in `Begin`
        // dereferences a null font, which is a segmentation fault and not a test failure.
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        REQUIRE(pixels != nullptr);
        REQUIRE(width > 0);
    }
    ~Ui() { ImGui::DestroyContext(); }
};

// Created on first use, which is inside a test case: the font build below uses REQUIRE, and a
// REQUIRE during static initialisation is outside any test context.
Ui& ui() {
    static Ui instance;
    return instance;
}

// One frame of the panel, with the device numbers a real frame would carry. The panel drains its
// instrument queue inside `draw`, so drawing is what a running app does to apply a switch.
void draw(Engine& engine, const ParamsPanel::Instruments& controls, ParamsPanel& panel) {
    boreaz::gui::DeviceStats device;
    device.open = true;
    device.sample_rate = fixture::kRate;
    boreaz::gui::MidiStats midi;
    const FrameView view = boreaz::gui::make_frame_view(engine, std::move(device), std::move(midi));
    ImGui::NewFrame();
    panel.draw(engine, view, controls);
    ImGui::Render();
}

ParamsPanel::Instruments controls_for(const Instrument* library, int& applied,
                                      const Instrument*& applied_to) {
    ParamsPanel::Instruments controls;
    controls.load = [library](const std::string& path, std::string& reason) -> const Instrument* {
        if (path.empty()) {
            reason = "no path given";
            return nullptr;
        }
        return library;
    };
    controls.applied = [&](const Instrument* instrument) {
        ++applied;
        applied_to = instrument;
    };
    return controls;
}

} // namespace

TEST_CASE("an instrument asked for while a note is sounding waits for the note to stop") {
    ui();
    Engine engine;
    engine.prepare(fixture::kRate);

    FakeInstrument library;
    int applied = 0;
    const Instrument* applied_to = nullptr;
    ParamsPanel::Instruments controls = controls_for(&library, applied, applied_to);

    ParamsPanel panel;
    panel.toggle(); // the panel draws only while it is open
    draw(engine, controls, panel);

    REQUIRE(engine.push_event(Event::note_on(0, 0, 60, 110)));
    fixture::render(engine, 4096);
    REQUIRE(engine.active_voices() > 0);

    panel.request(&library);
    for (int frame = 0; frame < 5; ++frame)
        draw(engine, controls, panel);

    // The note is still ringing. Applying the switch here is the bug: it rebuilds the pool and the
    // note ends where the player never released the key.
    CHECK(applied == 0);
    CHECK(applied_to == nullptr);

    // The key comes up, the tail runs out, and the queue has to drain by itself - nothing else
    // tells it to - and once only, because a switch applied per frame would be a new voice pool per
    // frame.
    REQUIRE(engine.push_event(Event::note_off(0, 0, 60)));
    for (int block = 0; block < 200 && engine.active_voices() > 0; ++block)
        fixture::render(engine, 4096);
    REQUIRE(engine.active_voices() == 0);

    for (int frame = 0; frame < 10; ++frame)
        draw(engine, controls, panel);

    CHECK(applied == 1);
    CHECK(applied_to == &library);
}

TEST_CASE("going back to the synthesiser is a request like any other, and waits too") {
    ui();
    Engine engine;
    engine.prepare(fixture::kRate);

    FakeInstrument library;
    int applied = 0;
    const Instrument* applied_to = &library;
    ParamsPanel::Instruments controls = controls_for(&library, applied, applied_to);

    ParamsPanel panel;
    panel.toggle();
    draw(engine, controls, panel);

    panel.request(nullptr); // nullptr is the synthesiser
    draw(engine, controls, panel);
    CHECK(applied == 1);
    CHECK(applied_to == nullptr);

    // A second request with notes sounding is still waiting, so the count does not move.
    REQUIRE(engine.push_event(Event::note_on(0, 0, 60, 110)));
    fixture::render(engine, 4096);
    REQUIRE(engine.active_voices() > 0);
    panel.request(&library);
    for (int frame = 0; frame < 5; ++frame)
        draw(engine, controls, panel);
    CHECK(applied == 1);
}
