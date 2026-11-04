// The mixer panel's one rule that could break the audio path: an instrument the player asked for is
// not installed while notes are sounding.
//
// Everything else the panel does is a slider writing into `Params`, which is tested through the
// table in test_live_params.cpp - the panel reads that same table, so a parameter in the table is a
// parameter the panel reaches. What is tested here is the queue in front of
// `Engine::set_instrument`, because that call rebuilds the voice pool: allocations, deletes, and
// the end of every note that was still ringing. Two things stand in front of it: the Load button,
// in the next case, and a patch row, which asks for the library the patch's header names.

#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <imgui.h>

#include "boreaz/config/params_file.h"
#include "boreaz/engine/engine.h"
#include "boreaz/engine/instrument.h"
#include "boreaz/engine/param_table.h"
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

// A patch is not only numbers. Its header names the sampled library the voice wants, and that name
// is the only way the app's voice kind travels at all: there is no `voice_kind` row in the table,
// and a stored kind would be a lie the moment its library was gone. A browser row reaches
// `adopt_patch`; this suite has no way to click, so the case calls what a click calls.
TEST_CASE("a patch carries its voice kind: the library it names, or the reason it did not arrive") {
    ui();
    Engine engine;
    engine.prepare(fixture::kRate);

    FakeInstrument library;
    std::vector<std::string> asked;
    int applied = 0;
    const Instrument* applied_to = nullptr;
    ParamsPanel::Instruments controls;
    controls.load = [&](const std::string& path, std::string& reason) -> const Instrument* {
        asked.push_back(path);
        if (path == "gone/library.txt") {
            reason = "no such library on this machine";
            return nullptr;
        }
        return path.empty() ? nullptr : &library; // an empty path is the synthesiser, not an error
    };
    controls.applied = [&](const Instrument* instrument) {
        ++applied;
        applied_to = instrument;
    };

    const auto dir = std::filesystem::temp_directory_path() / "boreaz_patch_voice_kind";
    std::filesystem::create_directories(dir);
    const auto write_patch = [&](const char* file, const char* instrument) {
        boreaz::engine::Params shaped{};
        const boreaz::engine::ParamSpec* spec = boreaz::engine::param_spec("tuning_reference_hz");
        REQUIRE(spec != nullptr);
        boreaz::engine::param_write(*spec, 432.0f, shaped);
        std::string error;
        REQUIRE(boreaz::config::save_params_file((dir / file).string(), shaped, "Kind test",
                                                 instrument, error));
        // The row is built the way the browser builds one: the header is read before anything
        // plays.
        ParamsPanel::PatchEntry entry;
        entry.path = (dir / file).string();
        std::string reason;
        REQUIRE(boreaz::config::read_patch_meta(entry.path, entry.name, entry.instrument, reason));
        entry.label = entry.name;
        return entry;
    };

    ParamsPanel panel;
    panel.toggle();
    draw(engine, controls,
         panel); // the first frame takes the engine's parameters, as a real one does

    // A patch that names a library asks for it by that name, and the kind follows once the pool may
    // be rebuilt. The tuning row went in at the same moment: one click, one sound.
    const auto sampled = write_patch("sampled.txt", "assets/instruments/grand/library.txt");
    panel.adopt_patch(sampled, engine, controls);
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] == "assets/instruments/grand/library.txt");
    CHECK(panel.status() == "loaded Kind test"); // the row's line, before the pool has moved
    for (int frame = 0; frame < 3; ++frame)
        draw(engine, controls, panel);
    CHECK(applied == 1);
    CHECK(applied_to == &library);
    CHECK(panel.last_patch() == sampled.path);
    CHECK(panel.status() == "playing the loaded library"); // and the line that says it arrived
    CHECK(engine.params().tuning_reference_hz == doctest::Approx(432.0f));

    // A patch that names nothing says nothing about the instrument: the library that is playing
    // keeps playing, so the synthesiser is not smuggled in behind a preset.
    const auto bare = write_patch("bare.txt", "");
    panel.adopt_patch(bare, engine, controls);
    CHECK(asked.size() == 1);
    CHECK(applied == 1);

    // A patch whose library has gone away: the parameters still arrive, the kind does not change,
    // no second voice pool is built while the first one is playing, and the reason is a line in the
    // panel rather than a silence.
    const auto broken = write_patch("broken.txt", "gone/library.txt");
    panel.adopt_patch(broken, engine, controls);
    REQUIRE(asked.size() == 2);
    CHECK(asked[1] == "gone/library.txt");
    CHECK(applied == 1);
    CHECK(panel.status() == "no such library on this machine");
    CHECK(panel.last_patch() == broken.path);
    CHECK(engine.params().tuning_reference_hz == doctest::Approx(432.0f));
}
