// The Direct3D 11 + GLFW + Dear ImGui host. This is the only translation unit in the program that
// includes any of them, which is the point of AppWindow being a class with a pimpl: the engine, the
// device, and the ports link and run with no window system present, and so does this file's test
// coverage of the parts that matter (geometry, frame snapshots, the panel table) without one.
#define INITGUID                                                                                   \
    1 // define the COM interface IDs in this translation unit: __uuidof, which the
      // DXGI helpers use, is a Microsoft extension and this project builds with GCC
#define WIN32_LEAN_AND_MEAN 1
#define NOMINMAX 1

#include "boreaz/gui/app_window.h"

#include "boreaz/gui/computer_keyboard.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "boreaz/io/directory.h"

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
constexpr float kMinimumKeyboardHeight = 72.0f;
constexpr float kMaximumKeyboardHeight = 150.0f;
constexpr float kKeyboardShare = 0.36f;
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

    // A file the player dropped on the window. GLFW gives the callback C strings that are only good
    // for the duration of the call, so the name is copied here and the drawing frame is what acts
    // on it - the callback has no app to hand it to and no business opening files itself. One slot:
    // a second drop before the frame draws replaces the first, which is what dropping twice means.
    std::string dropped;

    // The path the player is typing, and what the last attempt said about it. The buffer lives here
    // so a name typed a character at a time survives the 60 frames between keystrokes, and the note
    // is empty until something refuses a file: a load that worked has nothing left to report.
    bool choosing_path = false;
    bool focus_path = false; // the frame the field opens is the frame it takes the keyboard
    char path_field[512] = {0};
    std::string song_note;

    // The folder the player is looking through for a song, and what it holds. Read on the click that
    // asks for it and kept here, never taken every frame: the thread that pumps this window is the
    // thread that feeds the audio ring, and a directory walk on that thread is a stutter you hear.
    bool browsing = false;
    std::string browse_folder;
    std::string browse_note; // what a folder said when it refused to be listed
    std::vector<boreaz::io::FolderEntry> browse_entries;
    std::size_t browse_more = 0;
    int browse_pick = -1; // the row the arrow keys are on; -1 is none of them
    ImVec2 browse_at{0.0f, 0.0f}; // where the Browse button stood when it was pressed
};

// Rows the explorer draws at most. It is not a claim about what the folder holds - the count past it
// is printed, and typing the rest of a name reaches the rest of the list. It exists because one
// folder on a music machine holds 30 000 files and the listing is read on the audio thread's thread.
constexpr std::size_t kBrowseRows = 300;

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

void file_dropped(GLFWwindow* window, int count, const char** paths) {
    (void)window;
    if (g_app_window_impl == nullptr || count == 0 || paths == nullptr || paths[0] == nullptr)
        return;
    // Only the first name: this app plays one song at a time, so a two-file drop has no second
    // answer that is not a guess.
    g_app_window_impl->dropped = paths[0];
}
} // namespace

AppWindow::AppWindow() = default;
AppWindow::~AppWindow() {
    close();
}

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

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Direct3D presents to the surface itself
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window =
        glfwCreateWindow(options_.width, options_.height, options_.title.c_str(), nullptr, nullptr);
    if (window == nullptr) {
        error = "no window. The last GLFW error above says why.";
        return false;
    }
    impl_->window = window;
    glfwGetFramebufferSize(window, &impl_->width, &impl_->height);
    glfwSetFramebufferSizeCallback(window, &framebuffers_resized);
    glfwSetKeyCallback(window, &key_typed);
    glfwSetDropCallback(window, &file_dropped);

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
    result = impl_->swap_chain->GetBuffer(0, IID_ID3D11Texture2D, reinterpret_cast<void**>(&back));
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

void AppWindow::set_external_playback(bool playing) {
    external_playback_ = playing;
}

void AppWindow::park_transport() {
    transport_.halt();
}

void AppWindow::set_song_path(const std::string& path) {
    song_path_ = path;
}

void AppWindow::set_spectrum(const SpectrumSnapshot& spectrum) {
    spectrum_ = spectrum;
}

void AppWindow::draw(boreaz::engine::Engine& engine, const StatsFn& stats) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2{0.0f, 0.0f});
    ImGui::SetNextWindowSize(display);
    ImGui::Begin("##boreaz", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

    // Opening and closing the path field, reached from the button or from F1. It opens
    // prefilled with what is loaded, so replacing a song is editing a name rather than retyping it.
    const auto toggle_path_field = [this] {
        impl_->choosing_path = !impl_->choosing_path;
        if (!impl_->choosing_path)
            return;
        impl_->focus_path = true;
        std::snprintf(impl_->path_field, sizeof impl_->path_field, "%s", song_path_.c_str());
    };

    // The folder listing. One walk per click, and a walk that failed leaves the listing that worked
    // on screen with the reason under it: a junction the player has no business reading is not a
    // reason to lose the folder they were standing in.
    const auto show_folder = [this](const std::string& folder) -> bool {
        std::string reason;
        std::vector<boreaz::io::FolderEntry> entries;
        std::size_t more = 0;
        if (!boreaz::io::list_folder(folder, entries, kBrowseRows, more, reason)) {
            impl_->browse_note = reason;
            return false;
        }
        impl_->browse_note.clear();
        impl_->browse_folder = folder;
        impl_->browse_entries = std::move(entries);
        impl_->browse_more = more;
        impl_->browse_pick = -1;
        return true;
    };
    const auto toggle_browse = [this, &show_folder] {
        impl_->browsing = !impl_->browsing;
        if (!impl_->browsing)
            return;
        if (!impl_->choosing_path)
            // F3 with no path field open: the explorer goes where the field's row is, because the
            // position the Browse button set is from a previous visit, and never set at all it is
            // (0, 0) and the window lands in the corner of the screen.
            impl_->browse_at = ImVec2{ImGui::GetWindowPos().x + 24.0f,
                                      ImGui::GetWindowPos().y + 120.0f};
        // Open where the song is, or where the name being typed will be. A name that is itself a
        // folder opens as itself, which is what typing `D:\songs` and pressing Browse means.
        const std::string seed =
            impl_->path_field[0] != '\0' ? std::string{impl_->path_field} : song_path_;
        if (show_folder(seed))
            return;
        // A bare name or an empty field means the folder the app is standing in, and it has to be the
        // absolute one: "." lists that folder but has no parent, so Left answers the same listing
        // twice and a player who was walking up has found a wall.
        const std::string parent = boreaz::io::parent_folder(seed);
        if (!show_folder(parent == "." ? boreaz::io::current_folder() : parent))
            show_folder(boreaz::io::current_folder());
    };

    DeviceStats device;
    MidiStats midi;
    if (stats)
        stats(device, midi);
    FrameView view = make_frame_view(engine, std::move(device), std::move(midi));
    view.frames = frames_;
    view.seconds = seconds_elapsed();
    // Measured on the app's thread, one frame behind the sound it came from, which is the same delay
    // every other number on this window has: the alternative is the display reaching into the audio path.
    view.spectrum = spectrum_;

    // The transport starts on music and not on the driver: a file the app is playing, or a note
    // sounding right now. A file counts on its own because its first events are often controller
    // messages with no voice in them, and a clock opened at the first note would be behind by
    // whatever came before it.
    transport_.update(view.position_seconds(), view.active_voices > 0 || external_playback_);
    view.transport_running = transport_.running();
    view.transport_seconds = transport_.seconds();

    // What has been played, in the same clock the strip scrolls by. Once per frame, before any
    // panel draws, and here rather than inside a panel because it is the one part of the display
    // that has to remember the last frame.
    trail_.update(view);

    // The switcher. Every panel is one button and nothing else in the program knows how many
    // there are; the tooltip is what tells the player that this one is a view, not a remote.
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
    if (ImGui::Button("Mixer"))
        mixer_.toggle();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Every parameter the engine has, on sliders. The rows come from the same table the "
            "parameter file is read from, so nothing here can disagree with config/params.ini.");
    ImGui::SameLine();
    ImGui::TextUnformatted(impl_->driver.c_str());

    // The player's controls, on a line of their own under the views. They are here rather than in
    // the mixer because the mixer is opened to build a sound and closed to play one, and a play
    // button you have to open a window to find is a play button you do not press. Everything on it
    // is drawn from what the app says and changes the app by asking: the song, the playhead and the
    // clock belong to the app, and the bar is a click, not a drag, because a bar that follows a
    // playhead and is also the thing you drag fights the player every frame.
    const Options::Transport& player = options_.transport;
    if (player.active && player.active()) {
        if (ImGui::Button(player.playing && player.playing() ? "Pause" : "Play",
                          ImVec2(72.0f, 0.0f)))
            player.toggle();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Play or pause (Enter). Paused at the end, play starts again.");

        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(64.0f, 0.0f)))
            player.stop();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Silence, and the playhead back to the top.");

        ImGui::SameLine();
        bool looping = player.looped && player.looped(); // not const: the checkbox wants to write
        if (ImGui::Checkbox("Loop", &looping))
            player.toggle_loop();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Repeat the song from its own top.");

        ImGui::SameLine();
        const double at = player.position ? player.position() : 0.0;
        const double last = player.end ? player.end() : 0.0;
        const float fraction =
            last > 0.0
                ? static_cast<float>(at / last < 0.0 ? 0.0 : (at / last > 1.0 ? 1.0 : at / last))
                : 0.0f;
        char clock_label[32];
        std::snprintf(clock_label, sizeof clock_label, "%d:%02d / %d:%02d",
                      static_cast<int>(at) / 60, static_cast<int>(at) % 60,
                      static_cast<int>(last) / 60, static_cast<int>(last) % 60);
        ImGui::ProgressBar(fraction, ImVec2(220.0f, 0.0f), clock_label);
        if (ImGui::IsItemClicked()) {
            const ImVec2 bar_min = ImGui::GetItemRectMin();
            const ImVec2 bar_max = ImGui::GetItemRectMax();
            const ImVec2 where = ImGui::GetMousePos();
            if (bar_max.x > bar_min.x && player.seek)
                player.seek((where.x - bar_min.x) / (bar_max.x - bar_min.x));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Click to jump to that place in the song.");
    }

    // The song itself: which file is loaded, and how to replace it without restarting the app.
    // There is deliberately no file *dialog*. GetOpenFileName runs a modal message loop of its own,
    // and the thread that pumps this window is the thread that feeds the event ring: a dialog left
    // open past the lookahead lets the device run past the samples it was given and stutter the
    // song. Typing a path or dropping a file on the window costs no thread at all.
    const auto ask_for_song = [this](const std::string& path) {
        std::string reason;
        if (options_.songs.load && options_.songs.load(path, reason)) {
            song_path_ = path;
            impl_->song_note.clear();
            impl_->choosing_path = false;
            impl_->browsing = false; // the file the player was looking for is the one that loaded
        } else {
            // The song that was playing keeps playing: a path that will not read is a typo, and a
            // typo has no business silencing a session.
            impl_->song_note =
                reason.empty() ? "nothing to load from that path" : std::move(reason);
        }
    };
    if (!impl_->dropped.empty()) {
        ask_for_song(impl_->dropped);
        impl_->dropped.clear();
    }

    // One row of the folder listing, from a click or from Enter. A folder is walked into; a file is
    // put in the field and asked for, which is the same thing the Load button means and the reason
    // there is one answer to "what does picking a song do" rather than two.
    const auto pick_folder_entry = [this, &show_folder, &ask_for_song](
                                       const boreaz::io::FolderEntry& entry) {
        if (entry.folder) {
            show_folder(entry.path);
            return;
        }
        std::snprintf(impl_->path_field, sizeof impl_->path_field, "%s", entry.path.c_str());
        ask_for_song(entry.path);
    };

    if (song_path_.empty())
        ImGui::TextUnformatted("song: none - drop a .mid file on this window");
    else
        ImGui::Text("song: %s", song_path_.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Change"))
        toggle_path_field();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Type a path and press Enter, or drop the file anywhere on this window "
                          "(F1 opens the path field). The song playing is replaced; a file that "
                          "will not read leaves it alone.");
    if (impl_->choosing_path) {
        ImGui::SameLine();
        // The field takes the keyboard in the frame it opens, or a hand that pressed F1 has to
        // reach for the mouse to finish what the shortcut started. Taking it every frame would
        // instead steal it back from any other field the player clicked into.
        if (impl_->focus_path) {
            ImGui::SetKeyboardFocusHere(0);
            impl_->focus_path = false;
        }
        // A path that will not read is said where it was typed: the field itself turns red and
        // the reason goes on the line under it. This app has no console window to read, so a
        // refusal that only reached stdout is a refusal the player never sees.
        const bool refused = !impl_->song_note.empty();
        if (refused) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.45f, 0.45f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4{0.34f, 0.10f, 0.10f, 1.0f});
        }
        const bool entered =
            ImGui::InputText("##song_path", impl_->path_field, sizeof impl_->path_field,
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (!impl_->song_note.empty())
            ImGui::PopStyleColor(2);
        ImGui::SameLine();
        const bool pressed = ImGui::Button("Load");
        ImGui::SameLine();
        if (ImGui::Button("Browse"))
            toggle_browse();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Look through the folders for a .mid or .midi file. Up and Down walk "
                              "the list, Enter picks what is highlighted, Left goes out one folder "
                              "and Escape closes it: F3 opens this without a mouse.");
        // Where the explorer goes: under the button that opened it, which is where the eye already is.
        impl_->browse_at = ImVec2{ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 6.0f};
        if (entered || pressed)
            ask_for_song(impl_->path_field);
        if (!impl_->song_note.empty())
            ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.45f, 1.0f}, "%s",
                               impl_->song_note.c_str());
    }
    if (!impl_->choosing_path && !impl_->song_note.empty()) {
        // No field open to hold the reason: a file dropped on the window, or one a shortcut named,
        // is refused in the line the player is looking at.
        ImGui::SameLine();
        ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.45f, 1.0f}, "%s", impl_->song_note.c_str());
    }

    // Both halves come from the window, in proportion. The keyboard takes a share of the height
    // and never more than a cap, because a maximised window should buy the strip room, not turn
    // middle C into a floor tile; whatever is left belongs to the panel, and the strip fills
    // all of it - the waterfall and the piano roll are going to want exactly this rectangle.
    const float below_switcher = ImGui::GetContentRegionAvail().y;
    const float keyboard_height =
        std::clamp(below_switcher * kKeyboardShare, kMinimumKeyboardHeight, kMaximumKeyboardHeight);
    const float panel_height = std::max(0.0f, below_switcher - keyboard_height - kWindowPadding);

    if (ImGui::BeginChild("panel", ImVec2(0.0f, panel_height), ImGuiChildFlags_None)) {
        panel(panel_).draw(view);
        ImGui::EndChild();
    }
    ImGui::Spacing();

    // Every hit test is arithmetic on these four numbers and no pixel position is ever stored,
    // which is the whole resize story: the layout is recomputed from the window every frame.
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    layout_ = KeyboardLayout{origin.x, origin.y, width, keyboard_height};
    ImGui::Dummy(ImVec2{width, keyboard_height});

    // A click becomes an event, and the event goes in at the audio thread's current position -
    // the same tick the MIDI thread stamps a hardware key with, so a click and a key on the
    // cable are indistinguishable to everything downstream, including the latency a player
    // hears.
    boreaz::engine::Event events[2];
    const std::size_t produced =
        keyboard_.draw(view, layout_, engine.sample_position(), options_.channel, events, 2);
    for (std::size_t index = 0; index < produced; ++index) {
        // A full ring is the engine's to count and the panel's to show; a UI thread cannot wait
        // for space, and trying would be the deadlock this design is built to avoid.
        engine.push_event(events[index]);
    }

    // The computer keyboard. The notes enter at the audio thread's current tick through the
    // same queue a click or a cable message uses, so nothing downstream can tell them apart,
    // which is the point: a desk with no instrument and no mouse still makes a sound. A key
    // press has no velocity, so it arrives at a firm 112 - not the 127 a maximum strike means,
    // and not the 96 that made every note played this way audibly softer than one clicked on
    // screen.

    // ImGui takes the characters of a text field - the library path, the name of a patch to
    // save - through its own input path and not through this queue, so typing "piano" into a
    // field used to play a chord as well, because every letter here is a note. WantTextInput is
    // the question that means it, and not WantCaptureKeyboard: keyboard navigation is off in
    // this app, so the wider flag would say "ImGui is typing" for reasons that have nothing to
    // do with a field and would take the piano away. The strokes are still drained, so closing
    // a field does not release a queue of notes that were typed while it was open.
    const bool typing = ImGui::GetIO().WantTextInput;

    while (impl_->stroke_count > 0) {
        const AppWindow::Impl::Stroke stroke = impl_->strokes[impl_->stroke_head];
        impl_->stroke_head = (impl_->stroke_head + 1) % AppWindow::Impl::kStrokes;
        --impl_->stroke_count;
        if (typing)
            continue;

        const std::int64_t tick = engine.sample_position();
        const int note = note_for_computer_key(stroke.key);
        if (note >= 0) {
            engine.push_event(stroke.action == GLFW_RELEASE
                                  ? boreaz::engine::Event::note_off(tick, options_.channel,
                                                                    static_cast<std::uint8_t>(note))
                                  : boreaz::engine::Event::note_on(tick, options_.channel,
                                                                   static_cast<std::uint8_t>(note),
                                                                   112));
            continue;
        }
        if (key_is_sustain_pedal(stroke.key)) {
            // Space is the pedal. Held down, not toggled, because that is the only thing a foot
            // knows, and the engine already knows how to hold a note through a pedal-down
            // release.
            engine.push_event(boreaz::engine::Event::control(
                tick, options_.channel, 64, stroke.action == GLFW_RELEASE ? 0 : 127));
            continue;
        }
        if (stroke.key == GLFW_KEY_BACKSPACE && stroke.action == GLFW_PRESS) {
            // Backspace sets the clock back to zero - time a scale run, read it, reset, run it
            // again. The window takes the key because the window owns the clock and a panel is
            // a view with no way to command anything, and it is not a letter because every
            // letter on this keyboard is a note: `r` is F#, so a letter shortcut would start a
            // sound as well as a stopwatch. This drops the bars already on the strip and starts
            // the window again at the playhead, which is what "from here" means - the strip's
            // own clock, which is not the song's playhead: the song's controls are the row
            // above and Enter.
            transport_.reset(view.position_seconds());
        }
        // F1 opens the path field. It is not a letter, so it costs no note: every letter on this
        // keyboard is a pitch, which is why the clock reset is Backspace and the transport is
        // Enter.
        if (stroke.key == GLFW_KEY_F1 && stroke.action == GLFW_PRESS)
            toggle_path_field();
        if (stroke.key == GLFW_KEY_F3 && stroke.action == GLFW_PRESS)
            toggle_browse(); // the folders without a mouse, for the same reason F1 is the field
        if (impl_->browsing && stroke.action == GLFW_PRESS) {
            // Up and Down walk the listing, Enter picks the highlighted row, Escape shuts the folder.
            // F-keys and arrows rather than letters because every letter on this keyboard is a pitch,
            // and a folder reached only by clicking is a folder a hand that is on the keys has never
            // seen - which is the same argument that put the path field on F1 and the views on F2.
            const int rows = static_cast<int>(impl_->browse_entries.size());
            if (stroke.key == GLFW_KEY_UP || stroke.key == GLFW_KEY_DOWN) {
                const int step = stroke.key == GLFW_KEY_DOWN ? 1 : -1;
                impl_->browse_pick =
                    rows == 0 ? -1
                    : impl_->browse_pick < 0 ? (step > 0 ? 0 : rows - 1)
                                             : (impl_->browse_pick + step + rows) % rows;
            } else if (stroke.key == GLFW_KEY_ESCAPE) {
                impl_->browsing = false;
            } else if (stroke.key == GLFW_KEY_LEFT) {
                show_folder(boreaz::io::parent_folder(impl_->browse_folder));
            } else if ((stroke.key == GLFW_KEY_ENTER || stroke.key == GLFW_KEY_KP_ENTER) &&
                       impl_->browse_pick >= 0 && impl_->browse_pick < rows) {
                pick_folder_entry(
                    impl_->browse_entries[static_cast<std::size_t>(impl_->browse_pick)]);
            }
        }
        if (stroke.key == GLFW_KEY_F2 && stroke.action == GLFW_PRESS) {
            // F2 moves to the next view. The switcher is a row of buttons, and a view reachable
            // only by clicking is a view a hand that is on the keys never opens, which is the same
            // reason the transport is Enter and the path field is F1. F-keys and Backspace are used
            // because every letter on this keyboard is a pitch: `w` is A#, so a letter shortcut
            // would sound a note as well as command. It walks the panel table, so a view added to
            // the table is reachable from here without touching this line. `table` and `count` are
            // the switcher's own, read earlier in this function: one table, one walk of it.
            const int next = (static_cast<int>(panel_) + 1) % count;
            panel_ = table[next].kind;
        }
        if ((stroke.key == GLFW_KEY_ENTER || stroke.key == GLFW_KEY_KP_ENTER) &&
            stroke.action == GLFW_PRESS && options_.transport.toggle && !impl_->choosing_path &&
            !impl_->browsing) {
            // Enter is the transport, and it calls the same thing the button calls: what one
            // press means is the transport's answer, not two answers that can disagree. It is
            // Enter rather than Space because Space is the sustain pedal, and it is not a
            // letter because a letter on this keyboard is a note - `m` is A, so a shortcut in
            // letters would sound as well as command. With the path field open the field owns
            // Enter: it means "load the name that was typed", which the text field itself
            // answers.
            options_.transport.toggle();
        }
    }

    // Focus can be lost with a mouse button down and no release ever delivered - Alt-Tab,
    // another window dragged over the top. The note would sound until the player clicked again.
    const bool focused = glfwGetWindowAttrib(impl_->window, GLFW_FOCUSED) == GLFW_TRUE;
    if (was_focused_ && !focused) {
        const std::size_t released =
            keyboard_.force_release(engine.sample_position(), options_.channel, events, 2);
        for (std::size_t index = 0; index < released; ++index)
            engine.push_event(events[index]);
    }
    was_focused_ = focused;

    ImGui::End();

    // The folder explorer, its own window like the mixer's: it is opened to find a file and closed to
    // play one. There is deliberately no system dialog here - see the comment above ask_for_song, the
    // one about the modal message loop and the thread that feeds the audio ring. What is here instead
    // is a listing the app read once, on the click that asked for it.
    if (impl_->browsing) {
        ImGui::SetNextWindowPos(impl_->browse_at, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2{420.0f, 320.0f}, ImGuiCond_Appearing);
        // NoNavInputs because the arrow keys and Enter in this window belong to the handler above,
        // which knows what a folder list means, and not to ImGui's own focus walker, which does not:
        // with both walking, one Enter press activates whatever ImGui had focused and whatever this
        // handler had highlighted, and a song loads from a row the player never chose.
        ImGui::Begin("song folders", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavInputs);
        ImGui::TextUnformatted(impl_->browse_folder.c_str());
        if (!impl_->browse_note.empty())
            ImGui::TextColored(ImVec4{1.0f, 0.45f, 0.45f, 1.0f}, "%s",
                               impl_->browse_note.c_str());
        ImGui::Separator();
        if (ImGui::SmallButton("Up one folder"))
            show_folder(boreaz::io::parent_folder(impl_->browse_folder));
        if (ImGui::SmallButton("Close"))
            impl_->browsing = false;
        ImGui::BeginChild("##rows", ImVec2{0.0f, 0.0f}, true);
        for (std::size_t index = 0; index < impl_->browse_entries.size(); ++index) {
            const boreaz::io::FolderEntry& entry = impl_->browse_entries[index];
            const std::string label = entry.folder ? entry.name + "/" : entry.name;
            if (ImGui::Selectable(label.c_str(),
                                  static_cast<int>(index) == impl_->browse_pick))
                pick_folder_entry(entry);
        }
        if (impl_->browse_more > 0) {
            char rest[64];
            std::snprintf(rest, sizeof rest, "...and at least %zu more. Type the rest of the name.",
                          impl_->browse_more);
            ImGui::TextUnformatted(rest);
        }
        if (impl_->browse_entries.empty() && impl_->browse_note.empty())
            ImGui::TextUnformatted("no .mid or .midi files here");
        ImGui::EndChild();
        ImGui::End();
    }

    // The mixer is a second window, drawn after the main one closes: it floats over the
    // keyboard on purpose, because it is opened to build a sound and closed to play one, and a
    // strip of sliders permanently above the keys is the clutter that was already asked for
    // less of.
    mixer_.draw(engine, view, options_.instruments);
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
    // One-sync presents, so the window is frame rate limited by the monitor and never races
    // ahead of the audio thread by burning CPU on frames nobody can see. Exactly once per
    // frame, and not inside a condition: `a == X || b == Y` with a Present on each side
    // presents twice whenever the device is healthy, and a second Present in the same frame
    // waits for a second vsync.
    const HRESULT presented = impl_->swap_chain->Present(1, 0);
    if (presented == DXGI_ERROR_DEVICE_REMOVED || presented == DXGI_ERROR_DEVICE_RESET) {
        // Deliberately not handled beyond saying so: the audio keeps running and the app exits
        // on the player's cue. A driver reset that re-creates the device is a real feature and
        // is not in the plan, and an unhandled case that announces itself beats one that goes
        // quiet.
        std::fprintf(stderr, "boreaz: the graphics device went away\n");
    }
    ++frames_;
    return glfwWindowShouldClose(impl_->window) == GLFW_FALSE;
}

} // namespace boreaz::gui
