// The Direct3D 11 + GLFW + Dear ImGui host. This is the only translation unit in the program that
// includes any of them, which is the point of AppWindow being a class with a pimpl: the engine, the
// device, and the ports link and run with no window system present, and so does this file's test
// coverage of the parts that matter (geometry, frame snapshots, the panel table) without one.
#define INITGUID 1  // define the COM interface IDs in this translation unit: __uuidof, which the
                    // DXGI helpers use, is a Microsoft extension and this project builds with GCC
#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1

#include "boreaz/gui/app_window.h"

#include "boreaz/gui/computer_keyboard.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include <d3d11.h>

#include <GLFW/glfw3.h>

// The native window handle is how Direct3D gets an HWND from a window it knows nothing about.
#define GLFW_EXPOSE_NATIVE_WIN32 1
#include <GLFW/glfw3native.h>

#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_glfw.h>
#include <imgui.h>

namespace boreaz::gui {
namespace {

// The strip gets the bottom of the window and at least this much height: below it, 88 keys are
// too narrow to aim at, and a strip that cannot reach that width has to scroll rather than shrink
// the keys, which is the same promise the plan makes about window resizing.
constexpr float kPanelHeight = 190.0f;
constexpr float kMinimumKeyboardHeight = 96.0f;
constexpr float kWindowPadding = 12.0f;

void glfw_error(int code, const char* description) {
    std::fprintf(stderr, "boreaz: window system error %d: %s\n", code, description);
}

} // namespace

// One window per process, which is the whole application. GLFW's callbacks are plain C function
// pointers, and reaching the surface through a user pointer would mean making Impl a public type
// for no other reason. A second AppWindow in the same process would fight over this, and the plan
// says there is one window.
AppWindow::Impl* g_app_window_impl = nullptr;

struct AppWindow::Impl {
    GLFWwindow* window = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    int width = 0;
    int height = 0;
    bool resize_pending = false;
    bool glfw_started = false;
    bool gui_started = false;
    std::string driver;
    std::chrono::steady_clock::time_point started{};

    // Key presses waiting to become notes. GLFW runs its callbacks on the thread that pumps events,
    // which is the thread that draws, so this queue needs no lock - and it has to be a queue rather
    // than a direct push because the callback has no engine to push to.
    struct Stroke {
        int key = 0;
        int action = 0;
    };
    static constexpr int kStrokes = 32;
    Stroke strokes[kStrokes];
    int stroke_head = 0;
    int stroke_count = 0;
};

namespace {
void framebuffers_resized(GLFWwindow* window, int width, int height) {
    (void)window;
    if (g_app_window_impl == nullptr)
        return;
    g_app_window_impl->width = width;
    g_app_window_impl->height = height;
    // A minimise asks for a zero sized back buffer, which ResizeBuffers refuses. The flag is
    // cleared when a real size arrives, and Present in the meantime draws the old contents.
    if (width > 0 && height > 0)
        g_app_window_impl->resize_pending = true;
}

void key_typed(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)window;
    (void)scancode;
    (void)mods;
    AppWindow::Impl* impl = g_app_window_impl;
    if (impl == nullptr)
        return;
    // A held key repeats, and a repeat is not another keystroke: eleven note_ons for one press is
    // a voice-steal storm that reads as the synth glitching.
    if (action == GLFW_REPEAT)
        return;
    if (impl->stroke_count == AppWindow::Impl::kStrokes) {
        // The queue only fills if the window stops drawing, and a window that stops drawing has
        // bigger problems than a dropped keystroke. Overwrite the oldest and carry on.
        impl->stroke_head = (impl->stroke_head + 1) % AppWindow::Impl::kStrokes;
        --impl->stroke_count;
    }
    const int slot = (impl->stroke_head + impl->stroke_count) % AppWindow::Impl::kStrokes;
    impl->strokes[slot] = {key, action};
    ++impl->stroke_count;
}
} // namespace

AppWindow::AppWindow() = default;
AppWindow::~AppWindow() { close(); }

double AppWindow::seconds_elapsed() const {
    if (!impl_)
        return 0.0;
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - impl_->started).count();
}

bool AppWindow::create_surface(std::string& error) {
    glfwSetErrorCallback(&glfw_error);
    if (!glfwInit()) {
        error = "GLFW could not start. On a headless machine there is no window to open.";
        return false;
    }
    impl_->glfw_started = true;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Direct3D presents to the surface itself
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(options_.width, options_.height, options_.title.c_str(),
                                          nullptr, nullptr);
    if (window == nullptr) {
        error = "no window. The last GLFW error above says why.";
        return false;
    }
    impl_->window = window;
    glfwGetFramebufferSize(window, &impl_->width, &impl_->height);
    glfwSetFramebufferSizeCallback(window, &framebuffers_resized);
    glfwSetKeyCallback(window, &key_typed);

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = static_cast<UINT>(impl_->width);
    desc.BufferDesc.Height = static_cast<UINT>(impl_->height);
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 0;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = reinterpret_cast<HWND>(glfwGetWin32Window(window));
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    // Hardware first, then the Microsoft software rasteriser. WARP is slow and nobody wants it,
    // but a machine with a driver this code cannot open still gets an honest failure message
    // instead of silence, and a remote desktop session - where a lot of first runs happen - is
    // exactly the case where it is the only thing available.
    const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    HRESULT result = E_FAIL;
    for (const D3D_DRIVER_TYPE driver : drivers) {
        ID3D11Device* device = nullptr;
        ID3D11DeviceContext* context = nullptr;
        IDXGISwapChain* swap_chain = nullptr;
        result = D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, 0, nullptr, 0,
                                              D3D11_SDK_VERSION, &desc, &swap_chain, &device,
                                              nullptr, &context);
        if (FAILED(result))
            continue;
        impl_->device = device;
        impl_->context = context;
        impl_->swap_chain = swap_chain;
        impl_->driver = driver == D3D_DRIVER_TYPE_HARDWARE ? "Direct3D 11 hardware"
                                                           : "Direct3D 11 WARP (software)";
        break;
    }
    if (FAILED(result)) {
        char detail[96];
        std::snprintf(detail, sizeof(detail), "0x%08lX", static_cast<unsigned long>(result));
        error = std::string("no Direct3D 11 device (") + detail +
                "). This build draws with Direct3D; there is no software 2D fallback.";
        return false;
    }

    ID3D11Texture2D* back = nullptr;
    result = impl_->swap_chain->GetBuffer(0, IID_ID3D11Texture2D,
                                          reinterpret_cast<void**>(&back));
    if (SUCCEEDED(result)) {
        result = impl_->device->CreateRenderTargetView(back, nullptr, &impl_->target);
        back->Release();
    }
    if (FAILED(result)) {
        error = "could not make a render target for the window";
        return false;
    }
    return true;
}

void AppWindow::destroy_surface() {
    if (!impl_)
        return;
    // ImGui first: its backend holds device objects that must go before the device they came from.
    if (impl_->gui_started) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        impl_->gui_started = false;
    }
    if (impl_->target != nullptr)
        impl_->target->Release();
    if (impl_->swap_chain != nullptr)
        impl_->swap_chain->Release();
    if (impl_->context != nullptr)
        impl_->context->Release();
    if (impl_->device != nullptr)
        impl_->device->Release();
    if (impl_->window != nullptr)
        glfwDestroyWindow(impl_->window);
    if (impl_->glfw_started)
        glfwTerminate();
    if (g_app_window_impl == impl_.get())
        g_app_window_impl = nullptr;
    impl_->target = nullptr;
    impl_->swap_chain = nullptr;
    impl_->context = nullptr;
    impl_->device = nullptr;
    impl_->window = nullptr;
}

bool AppWindow::open(const Options& options, std::string& error) {
    options_ = options;
    impl_ = std::make_unique<Impl>();
    impl_->started = std::chrono::steady_clock::now();
    g_app_window_impl = impl_.get();

    if (!create_surface(error)) {
        destroy_surface();
        impl_.reset();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No imgui.ini next to the executable. A program that writes a file nobody asked it to write,
    // in a directory the player did not choose, is a bug users find later.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplGlfw_InitForOther(impl_->window, true) ||
        !ImGui_ImplDX11_Init(impl_->device, impl_->context)) {
        error = "Dear ImGui could not start on this device";
        destroy_surface();
        impl_.reset();
        return false;
    }
    impl_->gui_started = true;
    return true;
}

void AppWindow::close() {
    if (!impl_)
        return;
    destroy_surface();
    impl_.reset();
}

void AppWindow::draw(boreaz::engine::Engine& engine, const StatsFn& stats) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
    ImGui::SetNextWindowSize(display);
    ImGui::Begin("##boreaz", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

    DeviceStats device;
    MidiStats midi;
    if (stats)
        stats(device, midi);
    FrameView view = make_frame_view(engine, std::move(device), std::move(midi));
    view.frames = frames_;
    view.seconds = seconds_elapsed();

    // The switcher. Every panel is one button and nothing else in the program knows how many
    // there are; the tooltip is what tells the player that this one is a readout, not a remote.
    int count = 0;
    const PanelEntry* table = panel_table(&count);
    for (int index = 0; index < count; ++index) {
        if (index > 0)
            ImGui::SameLine();
        if (ImGui::Button(table[index].label))
            panel_ = table[index].kind;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", table[index].hint);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(impl_->driver.c_str());

    if (ImGui::BeginChild("panel", ImVec2(0.0f, kPanelHeight), ImGuiChildFlags_None)) {
        panel(panel_).draw(view);
        ImGui::EndChild();
    }
    ImGui::Spacing();

    // The strip owns whatever height is left, and its geometry is recomputed from the window size
    // every frame: that is the whole resize story, because every hit test is arithmetic on these
    // four numbers and no pixel position is ever stored.
    const float available = ImGui::GetContentRegionAvail().y;
    const float keyboard_height = std::max(kMinimumKeyboardHeight, available - kWindowPadding);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    layout_ = KeyboardLayout{origin.x, origin.y, width, keyboard_height};
    ImGui::Dummy(ImVec2{width, keyboard_height});

    // A click becomes an event, and the event goes in at the audio thread's current position -
    // the same tick the MIDI thread stamps a hardware key with, so a click and a key on the cable
    // are indistinguishable to everything downstream, including the latency a player hears.
    boreaz::engine::Event events[2];
    const std::size_t produced =
        keyboard_.draw(view, layout_, engine.sample_position(), options_.channel, events, 2);
    for (std::size_t index = 0; index < produced; ++index) {
        // A full ring is the engine's to count and the panel's to show; a UI thread cannot wait
        // for space, and trying would be the deadlock this design is built to avoid.
        engine.push_event(events[index]);
    }

    // The computer keyboard. The notes enter at the audio thread's current tick through the same
    // queue a click or a cable message uses, so nothing downstream can tell them apart, which is
    // the point: a desk with no instrument and no mouse still makes a sound. A key press has no
    // velocity, so it arrives at a middling 96 rather than the 127 a maximum strike would mean.
    while (impl_->stroke_count > 0) {
        const AppWindow::Impl::Stroke stroke = impl_->strokes[impl_->stroke_head];
        impl_->stroke_head = (impl_->stroke_head + 1) % AppWindow::Impl::kStrokes;
        --impl_->stroke_count;

        const std::int64_t tick = engine.sample_position();
        const int note = note_for_computer_key(stroke.key);
        if (note >= 0) {
            engine.push_event(stroke.action == GLFW_RELEASE
                                  ? boreaz::engine::Event::note_off(
                                        tick, options_.channel, static_cast<std::uint8_t>(note))
                                  : boreaz::engine::Event::note_on(
                                        tick, options_.channel, static_cast<std::uint8_t>(note), 96));
            continue;
        }
        if (key_is_sustain_pedal(stroke.key)) {
            // Space is the pedal. Held down, not toggled, because that is the only thing a foot
            // knows, and the engine already knows how to hold a note through a pedal-down release.
            engine.push_event(boreaz::engine::Event::control(
                tick, options_.channel, 64, stroke.action == GLFW_RELEASE ? 0 : 127));
        }
    }

    // Focus can be lost with a mouse button down and no release ever delivered - Alt-Tab, another
    // window dragged over the top. The note would sound until the player clicked again.
    const bool focused = glfwGetWindowAttrib(impl_->window, GLFW_FOCUSED) == GLFW_TRUE;
    if (was_focused_ && !focused) {
        const std::size_t released =
            keyboard_.force_release(engine.sample_position(), options_.channel, events, 2);
        for (std::size_t index = 0; index < released; ++index)
            engine.push_event(events[index]);
    }
    was_focused_ = focused;

    ImGui::End();
}

bool AppWindow::frame(boreaz::engine::Engine& engine, const StatsFn& stats) {
    if (!impl_ || impl_->window == nullptr)
        return false;
    glfwPollEvents();

    if (impl_->resize_pending && impl_->width > 0 && impl_->height > 0) {
        impl_->resize_pending = false;
        ImGui_ImplDX11_InvalidateDeviceObjects();
        if (impl_->target != nullptr) {
            impl_->target->Release();
            impl_->target = nullptr;
        }
        HRESULT result = impl_->swap_chain->ResizeBuffers(0, static_cast<UINT>(impl_->width),
                                                          static_cast<UINT>(impl_->height),
                                                          DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(result)) {
            ID3D11Texture2D* back = nullptr;
            result = impl_->swap_chain->GetBuffer(0, IID_ID3D11Texture2D,
                                                  reinterpret_cast<void**>(&back));
            if (SUCCEEDED(result)) {
                result = impl_->device->CreateRenderTargetView(back, nullptr, &impl_->target);
                back->Release();
            }
        }
        if (FAILED(result))
            std::fprintf(stderr, "boreaz: resize failed; the window keeps the old size\n");
        ImGui_ImplDX11_CreateDeviceObjects();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw(engine, stats);

    ImGui::Render();
    if (impl_->target != nullptr) {
        const float background[4] = {0.08f, 0.08f, 0.09f, 1.0f};
        impl_->context->OMSetRenderTargets(1, &impl_->target, nullptr);
        impl_->context->ClearRenderTargetView(impl_->target, background);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    // One-sync presents, so the window is frame rate limited by the monitor and never races ahead
    // of the audio thread by burning CPU on frames nobody can see.
    if (impl_->swap_chain->Present(1, 0) == DXGI_ERROR_DEVICE_REMOVED ||
        impl_->swap_chain->Present(1, 0) == DXGI_ERROR_DEVICE_RESET) {
        // Deliberately not handled beyond saying so: the audio keeps running and the app exits on
        // the player's cue. A driver reset that re-creates the device is a real feature and is not
        // in the plan, and an unhandled case that announces itself beats one that goes quiet.
        std::fprintf(stderr, "boreaz: the graphics device went away\n");
    }
    ++frames_;
    return glfwWindowShouldClose(impl_->window) == GLFW_FALSE;
}

} // namespace boreaz::gui
