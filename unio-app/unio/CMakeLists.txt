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
    INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo_mark.png"
    OUTPUT "${UNIO_GENERATED_DIR}/logo_mark.h"
    SYMBOL logo_mark)
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
    src/orchestrator/peer_events.cpp
    src/orchestrator/cursor_router.cpp
    src/orchestrator/clipboard/clipboard_monitor.cpp
    src/orchestrator/control/protocol.cpp
    src/orchestrator/input/cursor_poller.cpp
    src/orchestrator/input/keycodes.cpp
    src/orchestrator/input/polled_motion_forwarder.cpp
    src/orchestrator/crypto/placeholder.cpp
    src/orchestrator/mock/mesh_crdt.cpp
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
    src/ui/activity_workspace_form.cpp
    src/ui/layout.cpp
    src/ui/layout_drag.cpp
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
        src/orchestrator/local_probe/local_probe_win32.cpp
        # Identify per-monitor overlay (GDI on WS_POPUP windows).
        src/platform/win32/identify_overlay_win32.cpp
        # Phase A control channel — Linux gets the real POSIX
        # TCP impl in the elseif branch below; Windows gets the
        # real Winsock impl (added next to this list).
        src/orchestrator/control/tcp_control_channel_win32.cpp
        # Phase B input backend — SendInput-based mouse event
        # synthesis on Windows.
        src/orchestrator/input/input_win32.cpp
        # Sibling helper: RawInput event-pump thread that
        # captures scroll wheel + keyboard for forwarding while
        # this peer is dormant.
        src/orchestrator/input/win32_raw_capture.cpp
        # Sibling helper: WH_MOUSE_LL + WH_KEYBOARD_LL hooks
        # that swallow local input while we're dormant so
        # clicks / keystrokes don't leak to local apps.
        src/orchestrator/input/win32_input_grab.cpp
        # Clipboard backend: Win32 OpenClipboard / SetClipboardData
        # with CF_UNICODETEXT, UTF-8 ↔ UTF-16 at the boundary.
        src/orchestrator/clipboard/clipboard_win32.cpp
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
        src/orchestrator/local_probe/local_probe_x11.cpp
        # Identify per-monitor overlay (override-redirect X11 windows
        # with libXft text).
        src/platform/x11/identify_overlay_x11.cpp
        # Peer-to-peer TCP control channel — Phase A is Linux-only;
        # a sibling tcp_control_channel_win32.cpp lands later.
        src/orchestrator/control/tcp_control_channel.cpp
        # X11 input backend — XQueryPointer + XWarpPointer +
        # libXtst (XTestFakeMotionEvent / XTestFakeButtonEvent).
        src/orchestrator/input/input_x11.cpp
        # Sibling helper: opens /dev/input/event* keyboard +
        # pointer nodes, reads input events into HID-mapped
        # callbacks, EVIOCGRAB toggles for kernel-level local
        # input suppression.
        src/orchestrator/input/evdev_capture.cpp
        # Clipboard backend: native X11 selection protocol on
        # its own Display* — no xclip / xsel shell-out.
        src/orchestrator/clipboard/clipboard_x11.cpp
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
    # probe's display enumeration; Xft renders the Identify
    # overlay's TrueType text on each monitor; Xtst lets the
    # input backend synthesise mouse events on the receive side.
    find_package(X11 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XFT REQUIRED IMPORTED_TARGET xft)
    target_link_libraries(unio-ui PRIVATE
        ${X11_LIBRARIES}
        Xrandr
        Xtst
        Xfixes
        Xi
        PkgConfig::XFT
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
