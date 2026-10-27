// Boreaz - 88-key audio/MIDI synthesizer.
//
// Phase 0 of the plan only proves the build: the binary links miniaudio, GLFW and Dear
// ImGui and reports what it was built against. No audio, MIDI, or window yet.

#include <cstdio>
#include <cstring>
#include <string_view>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <miniaudio.h>

#include "boreaz/version.h"

namespace {

void print_version() {
    int major = 0;
    int minor = 0;
    int patch = 0;
    glfwGetVersion(&major, &minor, &patch);

    std::printf("boreaz %s\n", boreaz::kVersionString);
    std::printf("  glfw      %d.%d.%d  %s\n", major, minor, patch, glfwGetVersionString());
    std::printf("  imgui     %s\n", ImGui::GetVersion());
    std::printf("  miniaudio %s\n", MA_VERSION_STRING);
}

void print_usage() {
    std::printf(
        "boreaz %s - 88-key audio/MIDI synthesizer\n"
        "\n"
        "usage: boreaz [option]\n"
        "\n"
        "  -v, --version   print version and linked dependency versions, then exit\n"
        "  -h, --help      print this help, then exit\n"
        "\n"
        "No other option is implemented yet: audio, MIDI, rendering and the window arrive in\n"
        "later phases (see PLAN.md). Configuration comes from the environment, optionally\n"
        "backed by a local .env file - see .env.example and docs/PRIVACY.md.\n",
        boreaz::kVersionString);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 1) {
        print_usage();
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "boreaz: expected at most one option\n");
        return 2;
    }

    const std::string_view option = argv[1];
    if (option == "--version" || option == "-v") {
        print_version();
        return 0;
    }
    if (option == "--help" || option == "-h") {
        print_usage();
        return 0;
    }

    std::fprintf(stderr, "boreaz: unknown option '%s' (try --help)\n", argv[1]);
    return 2;
}
