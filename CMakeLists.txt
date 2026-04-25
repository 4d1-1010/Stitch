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
#   * unio-app/     — desktop app (unio-ui + unio-license-tool)
#                     and its licensing tooling. Builds standalone
#                     via unio-app/packaging/docker/build-linux.sh;
#                     also wired in here for whole-repo configures.
#
# Target shape tomorrow (ARCHITECTURE.md §1): one final `unio`
# binary links unio_ui_core + unio_pipe_core + unio_orchestrator_core
# as static libraries. Keeping the top-level here means that future
# executable target has a natural home.

option(UNIO_BUILD_PIPE "Build unio-pipe binary (media path)" ON)
option(UNIO_BUILD_APP  "Build unio-app (UI + licensing CLI)" ON)

# Collect unio-pipe's executables in build/bin/ for repo-wide
# configures. unio-app pins its own outputs under unio-app/dist/
# (see unio-app/CMakeLists.txt) so it ships the same paths whether
# built standalone or as part of this whole-repo configure.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(UNIO_BUILD_PIPE)
    add_subdirectory(unio-pipe)
endif()

if(UNIO_BUILD_APP)
    add_subdirectory(unio-app)
endif()

# ── doxygen docs (opt-in via `cmake --build . --target docs`) ──
#
# Kept off the default build path so regular dev rebuilds aren't
# slowed by doc generation. Driven by ./Doxyfile at the repo root
# — that file pins everything except OUTPUT_DIRECTORY, which we
# set here so the docs always land in build/docs/html/.
#
# The docker Linux image (packaging/docker/Dockerfile.linux)
# already installs doxygen + graphviz; native builds need them
# on PATH.
find_package(Doxygen)
if(DOXYGEN_FOUND)
    set(DOXYGEN_OUT_DIR "${CMAKE_BINARY_DIR}/docs")
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DOXYGEN_OUT_DIR}"
        COMMAND ${CMAKE_COMMAND} -E env
            "OUTPUT_DIRECTORY=${DOXYGEN_OUT_DIR}"
            ${DOXYGEN_EXECUTABLE} "${CMAKE_SOURCE_DIR}/Doxyfile"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Generating API docs with doxygen → ${DOXYGEN_OUT_DIR}/html"
        VERBATIM)
else()
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E echo
            "doxygen not found; install it or use the docker image (packaging/docker/Dockerfile.linux)"
        COMMAND ${CMAKE_COMMAND} -E false)
endif()
