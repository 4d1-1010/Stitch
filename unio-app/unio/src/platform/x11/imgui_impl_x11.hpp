/// @file imgui_impl_x11.hpp
/// @brief ImGui platform backend driven by raw Xlib events.
///
/// Xlib types are forward-declared (rather than @c \#include'd)
/// so consumers don't drag X11's @c Status typedef into TUs that
/// later include @c imgui.h.
#pragma once

extern "C" {
struct _XDisplay;
union _XEvent;
}
using Display = struct _XDisplay;
using XID     = unsigned long;
using Window  = XID;
using XEvent  = union _XEvent;

namespace unio_ui::platform::x11 {

/// @brief Bind ImGui's platform IO to an X11 display + window.
/// Call after @c ImGui::CreateContext() and before the first
/// @c ImGui::NewFrame().
/// @return @c true on success.
bool imgui_impl_x11_init(Display* dpy, Window win);

/// @brief Release resources held by @ref imgui_impl_x11_init.
void imgui_impl_x11_shutdown();

/// @brief Translate one @c XEvent into ImGui IO state.
/// @return @c true if ImGui consumed the event.
bool imgui_impl_x11_process_event(const XEvent& ev);

/// @brief Update display size, delta time, and cursor shape.
/// Call once per frame, immediately before @c ImGui::NewFrame().
void imgui_impl_x11_new_frame();

}  // namespace unio_ui::platform::x11
