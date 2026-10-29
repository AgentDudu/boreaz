cmake_minimum_required(VERSION 3.25)

project(boreaz
        VERSION 0.1.0
        DESCRIPTION "88-key audio/MIDI synthesizer"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Single-config generators (Ninja) put the exe in the build root, which is where README
# says it is.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

option(BOREAZ_WERROR "Treat warnings as errors for Boreaz's own code" ON)
option(BOREAZ_BUILD_TESTS "Build the test suite" ON)

# Keep the version header and the project command from drifting apart.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/include/boreaz/version.h" _version_header)
if(NOT _version_header MATCHES "kVersionString = \"${PROJECT_VERSION}\"")
  message(FATAL_ERROR
          "include/boreaz/version.h does not say \"${PROJECT_VERSION}\" - bump both together")
endif()

# ---------------------------------------------------------------- dependencies
#
# Every dependency is pinned to a tag: a floating ref makes the build unbuildable
# by someone else in six months. Bumping one is a commit of its own.
include(FetchContent)

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG 0.11.25
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.93.0
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.5.3
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(glfw doctest)

# miniaudio and Dear ImGui ship no CMake lists, so their targets live here.
# FetchContent_Populate() is the pre-CMake-3.28 way in; the modern MakeAvailable() would
# insist on add_subdirectory(), which these two have no CMakeLists.txt for.
FetchContent_Populate(miniaudio)
FetchContent_Populate(imgui)

add_library(miniaudio STATIC ${miniaudio_SOURCE_DIR}/miniaudio.c)
target_include_directories(miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR})
target_compile_features(miniaudio PUBLIC c_std_99)
if(WIN32)
  target_link_libraries(miniaudio PUBLIC winmm)
endif()

add_library(
  imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR}
                                             ${imgui_SOURCE_DIR}/backends)
target_compile_features(imgui PUBLIC cxx_std_17)
target_link_libraries(imgui PUBLIC glfw)
if(WIN32)
  # Compiled now, used from phase 5 on: building the DX11 backend in phase 0 is what proves
  # the rendering stack exists before anything depends on it.
  target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)
endif()

# Third-party headers are not ours to make warning-clean; -Werror says otherwise.
function(boreaz_target_is_system target)
  if(TARGET ${target})
    get_target_property(_includes ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(_includes)
      set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
                                                  "${_includes}")
    endif()
  endif()
endfunction()
boreaz_target_is_system(glfw)
boreaz_target_is_system(doctest)

function(boreaz_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /external:W0)
    if(BOREAZ_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(
      ${target}
      PRIVATE -Wall
              -Wextra
              -Wpedantic
              -Wshadow
              -Wconversion
              -Wsign-conversion
              -Wold-style-cast
              -Wnon-virtual-dtor
              -Wcast-align
              -Woverloaded-virtual
              -Wunused)
    if(BOREAZ_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# Statically link the compiler runtime into our executables. A machine can have more than
# one MinGW runtime on PATH - this one has two, and the loader takes the first match - so a
# floating libstdc++-6.dll fails at load with 0xc0000139 long before any of our code runs.
# It also makes the shipped exe portable, which is the point of "lightweight".
function(boreaz_link_runtime_staticly target)
  if(MINGW)
    target_link_options(${target} PRIVATE -static -static-libgcc)
  endif()
endfunction()

# ---------------------------------------------------------------- synthesis
#
# Header-only: these are per-sample primitives that must inline into the voice loop, and an
# INTERFACE target is also the dependency rule made structural — boreaz_dsp links nothing, so
# it cannot accidentally grow an I/O dependency.
add_library(boreaz_dsp INTERFACE)
target_include_directories(boreaz_dsp INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(boreaz_dsp INTERFACE cxx_std_20)

# ---------------------------------------------------------------- config
#
# Parameter files: the field table, the parser, and where the instrument's own defaults live. Its
# own library because the app, the render tool and the tests all read the same files, and a table
# that exists twice is a preset that plays one way and renders another.
add_library(boreaz_config STATIC src/config/params_file.cpp)
target_include_directories(boreaz_config PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(boreaz_config PUBLIC boreaz_engine)
target_compile_features(boreaz_config PUBLIC cxx_std_20)
boreaz_set_warnings(boreaz_config)

# ---------------------------------------------------------------- engine
#
# Compiled, not header-only: a voice is real code with real state. A static target that links
# boreaz_dsp and nothing else is what makes the plan's dependency direction checkable - the
# engine has no miniaudio or ImGui include path at all, which is what keeps it testable
# headless.
add_library(
  boreaz_engine
  STATIC src/engine/voice.cpp src/engine/voice_allocator.cpp src/engine/engine.cpp
         src/engine/message.cpp)
target_include_directories(boreaz_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(boreaz_engine PUBLIC boreaz_dsp)
target_compile_features(boreaz_engine PUBLIC cxx_std_20)
boreaz_set_warnings(boreaz_engine)

# The rule as a build failure rather than a comment.
get_target_property(_boreaz_engine_links boreaz_engine INTERFACE_LINK_LIBRARIES)
if(_boreaz_engine_links MATCHES "miniaudio|imgui|glfw")
  message(FATAL_ERROR "boreaz_engine must not link an I/O or GUI library, got ${_boreaz_engine_links}")
endif()

# ---------------------------------------------------------------- i/o
#
# Devices and MIDI. This is the only Boreaz library allowed to reach a driver: miniaudio for the
# output, winmm for the keyboard. Note that miniaudio has no MIDI API at all, so MIDI In is our
# own thin winmm wrapper (see include/boreaz/io/midi_input.h).
add_library(
  boreaz_io
  STATIC src/io/midi_parser.cpp src/io/midi_input.cpp src/io/audio_device.cpp)
target_include_directories(boreaz_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(boreaz_io PUBLIC boreaz_engine miniaudio)
target_compile_features(boreaz_io PUBLIC cxx_std_20)
if(WIN32)
  target_link_libraries(boreaz_io PUBLIC winmm)
endif()
boreaz_set_warnings(boreaz_io)

# The GUI stays out of here: a phase 5 window must call into i/o, never the other way round, and
# that is what lets the same engine run headless.
get_target_property(_boreaz_io_links boreaz_io INTERFACE_LINK_LIBRARIES)
if(_boreaz_io_links MATCHES "imgui|glfw")
  message(FATAL_ERROR "boreaz_io must not link a GUI library, got ${_boreaz_io_links}")
endif()

# ---------------------------------------------------------------- midifile
#
# Standard MIDI File reading, shared by the offline renderer (phase 4) and the in-app sequencer
# (phase 10). It reads files and knows the tempo map; it may not touch a device, which is why it
# links the engine and stops there. The parser is ours rather than a dependency: PLAN.md §2 named
# `midicpp`, which turned out to be a SourceForge-era library with no licence text and no header
# only build, and the subset we accept is six message kinds - see the header.
add_library(boreaz_midifile STATIC src/midifile/midifile.cpp)
target_include_directories(boreaz_midifile PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(boreaz_midifile PUBLIC boreaz_engine)
target_compile_features(boreaz_midifile PUBLIC cxx_std_20)
boreaz_set_warnings(boreaz_midifile)

get_target_property(_boreaz_midifile_links boreaz_midifile INTERFACE_LINK_LIBRARIES)
if(_boreaz_midifile_links MATCHES "miniaudio|imgui|glfw|winmm")
  message(FATAL_ERROR "boreaz_midifile must not link a device library, got ${_boreaz_midifile_links}")
endif()

# ---------------------------------------------------------------- gui
#
# The window, the 88 keys, and the readout above them. Its own library, so the dependency direction
# is a build fact rather than a wish: boreaz_engine and boreaz_io link no ImGui, no GLFW, no
# window, and that is what lets the same synthesis run headless in a test and in a tool.
#
# Windows only, and that is a fact about the two things a window needs rather than laziness: the
# draw backend (Direct3D 11, which is what the ImGui target above compiles its backend against) and
# the host MIDI API. Porting means adding a backend file and a ports file for the next platform and
# widening this guard - not inventing an abstraction layer with one implementation in it, which the
# plan rejected on purpose.
if(WIN32)
  add_library(
    boreaz_gui
    STATIC src/gui/app_window.cpp src/gui/frame_view.cpp src/gui/keyboard_view.cpp
           src/gui/panel.cpp src/gui/viz/transport.cpp)
  target_include_directories(boreaz_gui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(boreaz_gui PUBLIC imgui boreaz_io boreaz_engine boreaz_dsp)
  target_compile_features(boreaz_gui PUBLIC cxx_std_20)
  boreaz_set_warnings(boreaz_gui)
endif()

# ---------------------------------------------------------------- application
if(WIN32)
  add_executable(boreaz src/app/main.cpp)
  target_include_directories(boreaz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(boreaz PRIVATE boreaz_gui boreaz_io boreaz_config boreaz_engine boreaz_dsp)
  boreaz_set_warnings(boreaz)
  boreaz_link_runtime_staticly(boreaz)
else()
  message(
    STATUS
      "boreaz: no window target on this platform yet - the instrument, the tools and the tests build without one"
  )
endif()

# ---------------------------------------------------------------- tools
#
# Offline tools that are not the app: a render is a file, a file has a checksum, and a checksum
# cannot say "it sounded fine to me". Built from the same libraries the app uses, so a render in
# CI is the same synthesis the user hears.
add_executable(boreaz-render src/tools/render/main.cpp)
target_include_directories(boreaz-render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(boreaz-render PRIVATE boreaz_midifile boreaz_config boreaz_engine boreaz_dsp)
boreaz_set_warnings(boreaz-render)
boreaz_link_runtime_staticly(boreaz-render)

# ---------------------------------------------------------------- tests
if(BOREAZ_BUILD_TESTS)
  enable_testing()

  # The GUI suites are Windows-only with the library they test. What they test is the headless half
  # of the GUI - which key a click hits, what a frame view copies, what the switcher offers - which
  # is where "all 88 keys clickable" is actually decided, and none of it needs a display.
  set(boreaz_test_sources
      tests/test_version.cpp
    tests/test_hygiene.cpp
    tests/test_tuning.cpp
    tests/test_oscillator.cpp
    tests/test_svf.cpp
    tests/test_envelope.cpp
    tests/test_gain.cpp
    tests/test_voice.cpp
    tests/test_voice_allocator.cpp
    tests/test_param_bus.cpp
    tests/test_message.cpp
    tests/test_engine.cpp
    tests/test_engine_events.cpp
    tests/test_keyboard_layout.cpp
    tests/test_midi_parser.cpp
    tests/test_midifile.cpp
    tests/test_io.cpp)
  if(WIN32)
    list(APPEND boreaz_test_sources tests/test_gui.cpp)
  endif()

  add_executable(boreaz_tests ${boreaz_test_sources})
  target_include_directories(boreaz_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_definitions(boreaz_tests PRIVATE BOREAZ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  # The render tests run the tool rather than reimplementing its argument handling, and the golden
  # tests read fixtures from the source tree, so both paths arrive at compile time.
  target_compile_definitions(
    boreaz_tests
    PRIVATE BOREAZ_RENDER_TOOL="$<TARGET_FILE:boreaz-render>"
            BOREAZ_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/midifile")
  target_link_libraries(
    boreaz_tests
    PRIVATE doctest::doctest boreaz_io boreaz_midifile boreaz_config boreaz_engine
            boreaz_dsp)
  if(WIN32)
    target_link_libraries(boreaz_tests PRIVATE boreaz_gui)
  endif()
  # A target-file generator expression says where a binary will be, it does not make CMake build it,
  # and the tool tests shell out to it.
  add_dependencies(boreaz_tests boreaz-render)
  # The param-bus test runs a real writer thread, which on MinGW needs the pthread shim that
  # find_package(THREADS) already located.
  if(TARGET Threads::Threads)
    target_link_libraries(boreaz_tests PRIVATE Threads::Threads)
  endif()
  boreaz_set_warnings(boreaz_tests)
  boreaz_link_runtime_staticly(boreaz_tests)

  add_test(NAME boreaz_tests COMMAND boreaz_tests)
endif()
