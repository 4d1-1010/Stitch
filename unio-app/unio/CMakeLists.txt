# unio — desktop runtime binary (Dear ImGui).
#
# Project posture (C++20, MSVC CRT, strict warnings, embed
# helpers) is set by the parent unio-app CMakeLists.txt. This
# file only describes the runtime binary itself plus the
# `unio::license_format` static library that sibling licensing/
# links to share the wire-format codec.

# ── Vendored Dear ImGui ────────────────────────────────────────
# Core lib only; backends are added per-platform on the unio
# target so they pick up our include paths + per-platform
# defines. See vendor/imgui/README.md for the pinning policy.
add_subdirectory(vendor/imgui)

# ── Embedded blobs (fonts + logo + vendor pubkey) ──────────────
#
# Keeps the single-file ship story (feedback_minimal_deps).
# Regenerated on every configure but only when inputs change.
# The vendor public key lives next to the licensing/ tooling that
# also uses the matching private half — a cross-subdir reference
# is the correct shape: the *file is owned by licensing/*, the
# *binary that embeds it is unio*. No duplication.
set(UNIO_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

embed_binary_as_header(
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/inter/Inter-Regular.ttf"
    OUTPUT "${UNIO_GENERATED_DIR}/font_inter_regular.h"
    SYMBOL font_inter_regular)
embed_binary_as_header(
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/inter/Inter-Bold.ttf"
    OUTPUT "${UNIO_GENERATED_DIR}/font_inter_bold.h"
    SYMBOL font_inter_bold)
embed_binary_as_header(
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo_mark_48.png"
    OUTPUT "${UNIO_GENERATED_DIR}/logo_mark_48.h"
    SYMBOL logo_mark_48)
embed_binary_as_header(
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/../licensing/dev/vendor_master.pub.raw"
    OUTPUT "${UNIO_GENERATED_DIR}/vendor_master_pub_raw.h"
    SYMBOL vendor_master_pub_raw)

# ── unio::license_format — shared wire-format library ──────────
#
# The codec serialises/deserialises licence tokens at the byte
# level. Both the runtime app (which verifies pasted tokens) and
# the unio-license-tool from sibling licensing/ (which signs
# tokens) must agree on the bytes byte-for-byte. Compiling once
# and linking from both binaries enforces that.
add_library(unio_license_format STATIC
    src/license/codec.cpp
)
target_include_directories(unio_license_format PUBLIC include)
target_compile_options(unio_license_format PRIVATE ${UNIO_APP_CXX_FLAGS})
add_library(unio::license_format ALIAS unio_license_format)

# ── Runtime binary source list ─────────────────────────────────
set(UNIO_SOURCES
    src/main.cpp

    src/theme/style.cpp
    src/theme/logo.cpp

    src/orchestrator/orchestrator.cpp
    src/orchestrator/crypto/placeholder.cpp
    src/orchestrator/mock/mesh_crdt.cpp
    src/orchestrator/mock/discovery.cpp
    src/orchestrator/mock/pairing_manager.cpp
    src/orchestrator/mock/control_connection.cpp
    src/orchestrator/mock/media_connection.cpp
    src/orchestrator/mock/path_selector.cpp
    src/orchestrator/mock/session_scheduler.cpp
    src/orchestrator/mock/workspace_manager.cpp

    # Real LAN discovery — replaces the mock at runtime via the
    # orchestrator factory. Per-OS socket + interface impls are
    # appended below in the platform-specific block.
    src/orchestrator/net/announce_codec.cpp
    src/orchestrator/net/lan_discovery.cpp

    src/ui/primitives.cpp
    src/ui/rail.cpp
    src/ui/shell.cpp
    src/ui/machine_color.cpp
    src/ui/access.cpp
    src/ui/activity.cpp
    src/ui/activity_workspaces.cpp
    src/ui/layout.cpp
    src/ui/placeholder.cpp

    src/util/image_loader.cpp

    src/license/vendor_key.cpp

    src/platform/stub/stream_window.cpp
    src/platform/stub/source_overlay.cpp
)

if(WIN32)
    list(APPEND UNIO_SOURCES
        src/platform/win32/app_win32.cpp
        vendor/imgui/backends/imgui_impl_win32.cpp
        vendor/imgui/backends/imgui_impl_dx11.cpp
        # Winsock + iphlpapi impls of the net/ subsystem.
        src/orchestrator/net/udp_socket_win32.cpp
        src/orchestrator/net/lan_interfaces_win32.cpp
        # Real local-probe via EnumDisplayMonitors.
        src/orchestrator/platform/local_probe_win32.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND UNIO_SOURCES
        src/platform/x11/app_x11.cpp
        # In-house X11 backend — upstream doesn't ship one (see
        # ARCHITECTURE.md §5 decision #1a).
        src/platform/x11/imgui_impl_x11.cpp
        vendor/imgui/backends/imgui_impl_opengl3.cpp
        # POSIX socket + getifaddrs impls of the net/ subsystem.
        src/orchestrator/net/udp_socket_posix.cpp
        src/orchestrator/net/lan_interfaces_posix.cpp
        # Real local-probe via X11 RandR.
        src/orchestrator/platform/local_probe_x11.cpp
    )
endif()

if(WIN32)
    # WIN32 target attribute → WinMain entry point, no console
    # window alongside the GUI.
    add_executable(unio-ui WIN32 ${UNIO_SOURCES})
else()
    add_executable(unio-ui ${UNIO_SOURCES})
endif()

target_compile_options(unio-ui PRIVATE ${UNIO_APP_CXX_FLAGS})
target_include_directories(unio-ui PRIVATE
    include
    src
    "${UNIO_GENERATED_DIR}")
target_link_libraries(unio-ui PRIVATE
    imgui
    unio::license_format)

if(WIN32)
    target_link_libraries(unio-ui PRIVATE
        d3d11 dxgi dxguid
        user32 gdi32 kernel32
        # net/ subsystem on Windows: Winsock (WSAStartup, sockets,
        # WSAPoll) + IP Helper (GetAdaptersAddresses).
        ws2_32 iphlpapi
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Raw X11 + EGL + desktop GL. No GLFW, no SDL — see
    # ARCHITECTURE.md §5 decision 1a. Xrandr powers the local
    # probe's display enumeration.
    find_package(X11 REQUIRED)
    target_link_libraries(unio-ui PRIVATE
        ${X11_LIBRARIES}
        Xrandr
        EGL
        GL
    )
    target_include_directories(unio-ui PRIVATE ${X11_INCLUDE_DIR})
endif()

# Strip in release builds on Linux — matches unio-pipe's ship
# story. Windows keeps symbols for future crash-dump work. The
# packaging/docker/build-linux.sh extract step runs another strip
# pass when extracting; this one runs for native dev builds too.
if(CMAKE_BUILD_TYPE STREQUAL "Release"
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_command(TARGET unio-ui POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:unio-ui>)
endif()
