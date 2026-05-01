cmake_minimum_required(VERSION 3.16)
project(xorio-app VERSION 0.1.0 LANGUAGES CXX)

# xorio-app — desktop runtime + licensing tooling.
#
# Subdirectories:
#   * xorio/    — Dear ImGui desktop binary (ARCHITECTURE.md §5).
#   * licensing/ — vendor key material + the xorio-license-tool
#                  CLI that signs tokens. Dev-side only; not
#                  shipped to end users.
#
# Self-contained: builds standalone (`cmake -S xorio-app -B build`)
# and slots into the repo-root CMakeLists.txt as a subdirectory.
# unio-pipe is *not* a build dependency here — scope is the app
# layer only.

# Project-wide language posture. Subdirs inherit; vendored sources
# (Dear ImGui) keep their own warning flags via per-target options.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC CRT — single-file ship, no VCRuntime redist
# alongside the binary. Mirrors unio-pipe's posture.
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Strict-warnings flag set, exposed for subdirs to consume on
# their own targets only.
if(NOT MSVC)
    set(XORIO_APP_CXX_FLAGS
        -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
        CACHE INTERNAL "xorio-app private compile flags")
else()
    set(XORIO_APP_CXX_FLAGS
        /W4 /WX /wd4324 /wd4201
        CACHE INTERNAL "xorio-app private compile flags")
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Per-platform check. Sub-subdirs use this guard to bail out
# loudly on platforms we don't ship for; we don't pin
# RUNTIME_OUTPUT_DIRECTORY at the CMake level because the
# Docker-driven build extracts from /build/bin/ to
# xorio-app/dist/<plat>/ as a separate step (run as the host user)
# and a CMake-side pin would race the build-volume ownership.
if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32))
    message(FATAL_ERROR
        "xorio-app: unsupported platform ${CMAKE_SYSTEM_NAME}.")
endif()

# Collect every executable in build/bin/ regardless of which
# subdir produced it. Set only when we are the top-level project;
# repo-root configures already do this themselves.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Shared cmake helpers consumed by both subdirs. Adding cmake/ to
# CMAKE_MODULE_PATH lets `include(EmbedBinary)` resolve without
# absolute paths from subdirectories.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(EmbedBinary)

add_subdirectory(xorio)
add_subdirectory(licensing)
