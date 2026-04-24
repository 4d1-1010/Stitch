/*! @file imgui_impl_x11.hpp
 *  @brief In-house ImGui platform backend for raw Xlib.
 *
 *  Upstream Dear ImGui does not ship an X11 backend — only GLFW,
 *  SDL, and platform-specific (Win32 / OSX / Android) ones. We
 *  own this file; see ARCHITECTURE.md §5 decision #1a.
 *
 *  Feature scope (first cut):
 *    - Mouse position, buttons, wheel
 *    - Keyboard (`XKeysymToString` mapping to `ImGuiKey`)
 *    - Focus in / out (keyboard events gated on focus)
 *    - Resize (viewport size)
 *    - Display-size (io.DisplaySize)
 *    - Clipboard read / write (UTF-8 via `XA_CLIPBOARD` +
 *      `UTF8_STRING`, same-process loop for now)
 *    - Mouse cursor shapes via `libXcursor`
 *
 *  Explicit TODOs (filed as separate issues before the port's
 *  Phase 0 closes):
 *    - IME / dead-key / compose-key text input (XIM)
 *    - HiDPI scaling (`Xft.dpi` / XRandR output DPI)
 *    - Async clipboard-paste (XConvertSelection → SelectionNotify)
 *    - Multi-viewport (ImGui docking branch)
 *
 *  @note This header intentionally forward-declares the two X11
 *  types it takes by value rather than `#include <X11/Xlib.h>`.
 *  Pulling Xlib in here transitively would make every TU that
 *  touches the UI layer see X11's global `Status` typedef, which
 *  `imgui.h` then `#undef`s — and the next Xutil.h / Xatom.h
 *  include in the same TU would fail to parse. Keeping Xlib
 *  confined to the .cpp avoids the conflict entirely.
 */
#pragma once

// Forward-declare Xlib types. Must match the typedefs in
// <X11/Xlib.h> exactly (unsigned long XID; Display is an opaque
// struct; XEvent is a union).
extern "C" {
struct _XDisplay;
union _XEvent;
}
using Display = struct _XDisplay;
using XID = unsigned long;
using Window = XID;
using XEvent = union _XEvent;

namespace unio_ui::platform::x11 {

/// Initialise ImGui's platform IO against this X11 display/window.
/// Must be called after `ImGui::CreateContext()` and before any
/// `ImGui::NewFrame()`. Returns `true` on success.
bool imgui_impl_x11_init(Display* dpy, Window win);

/// Release resources held by @ref imgui_impl_x11_init. Safe to
/// call before the Display is closed.
void imgui_impl_x11_shutdown();

/// Feed one `XEvent` into ImGui's IO.
/// @return `true` if ImGui consumed the event. Callers may still
/// want to process events ImGui consumed (for example, a motion
/// event that also drives their own picking logic).
bool imgui_impl_x11_process_event(const XEvent& ev);

/// Call once per frame, before `ImGui::NewFrame()`. Updates
/// display size, delta-time, and mouse-cursor shape.
void imgui_impl_x11_new_frame();

}  // namespace unio_ui::platform::x11
