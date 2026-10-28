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

# ---------------------------------------------------------------- application
add_executable(boreaz src/app/main.cpp)
target_include_directories(boreaz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(boreaz PRIVATE imgui miniaudio boreaz_dsp)
boreaz_set_warnings(boreaz)
boreaz_link_runtime_staticly(boreaz)

# ---------------------------------------------------------------- tests
if(BOREAZ_BUILD_TESTS)
  enable_testing()

  add_executable(
    boreaz_tests
    tests/test_version.cpp
    tests/test_hygiene.cpp
    tests/test_tuning.cpp
    tests/test_oscillator.cpp
    tests/test_svf.cpp
    tests/test_envelope.cpp
    tests/test_gain.cpp)
  target_include_directories(boreaz_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_definitions(boreaz_tests PRIVATE BOREAZ_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(boreaz_tests PRIVATE doctest::doctest boreaz_dsp)
  boreaz_set_warnings(boreaz_tests)
  boreaz_link_runtime_staticly(boreaz_tests)

  add_test(NAME boreaz_tests COMMAND boreaz_tests)
endif()
