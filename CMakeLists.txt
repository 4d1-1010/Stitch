cmake_minimum_required(VERSION 3.16)
project(unio VERSION 0.1.0 LANGUAGES C CXX)

# Top-level CMake for the whole repo. Drives the Docker build
# containers (packaging/docker/build-linux.sh + build-win.sh) and
# any native dev build.
#
# Each subdirectory stays self-contained — this file just wires
# them together. Shared settings (C++20, /MT on Windows, strict
# warnings) are intentionally NOT hoisted here: unio-pipe depends
# on many FetchContent targets (msquic, libvpl, openssl) whose own
# build flags we must not leak project-wide, and per-subdir scope
# keeps the blast radius of any flag change small.
#
# Target shape today:
#   * unio-pipe     — media-path helper, shipped standalone for
#                     tools/matrix_test.py + integration testing
#   * unio-ui       — UI layer scaffold (Dear ImGui; Win32+D3D11
#                     on Windows, Xlib+EGL+GL3 on Linux)
#
# Target shape tomorrow (ARCHITECTURE.md §1): one final `unio`
# binary links unio_ui_core + unio_pipe_core + unio_orchestrator_core
# as static libraries. Keeping the top-level here means that future
# executable target has a natural home.

option(UNIO_BUILD_PIPE "Build unio-pipe binary (media path)" ON)
option(UNIO_BUILD_UI   "Build unio-ui binary (UI layer)"     ON)

# Collect every subdir's executables in build/bin/ so packaging
# scripts don't have to know which subtree produced what.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(UNIO_BUILD_PIPE)
    add_subdirectory(unio-pipe)
endif()

if(UNIO_BUILD_UI)
    add_subdirectory(unio-ui)
endif()
