/// @file imgui_impl_x11.cpp
/// @brief ImGui platform backend driven by raw Xlib events.

// X11 headers are included before ImGui because ImGui `#undef`s
// `Status` to resolve a symbol collision. Subsequent X11 headers
// in the same TU need that typedef to parse.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include "imgui.h"
#include "imgui_impl_x11.hpp"

#include <chrono>
#include <cstring>
#include <string>

namespace xorio_ui::platform::x11 {

namespace {

// Context held behind ImGuiIO::BackendPlatformUserData. One per
// ImGui context (we only ever have one for the main window).
struct X11BackendContext {
    Display* dpy = nullptr;
    Window win = 0;

    // Cursor cache. Creating X cursors is moderately expensive;
    // keep one per ImGuiMouseCursor enum.
    Cursor cursors[ImGuiMouseCursor_COUNT] = {};
    Cursor current_cursor = None;

    // Selection ownership flags for clipboard. Tracked so the
    // helper thread-less `SetClipboardText` path can hold on to
    // the text until another client asks for it.
    Atom atom_clipboard = None;
    Atom atom_utf8 = None;
    Atom atom_targets = None;
    Atom atom_primary = XA_PRIMARY;
    std::string pending_clipboard_text;

    // Clipboard read. Walking the async Xlib selection protocol
    // from `GetClipboardText` needs a scratch buffer for the
    // reply.
    std::string last_pasted_text;

    // Time baseline for io.DeltaTime.
    std::chrono::steady_clock::time_point last_frame_time;

    bool focused = false;
};

X11BackendContext& ctx() {
    auto& io = ImGui::GetIO();
    // Allocated once in init, freed in shutdown.
    return *static_cast<X11BackendContext*>(io.BackendPlatformUserData);
}

// ── Key mapping ────────────────────────────────────────────────

ImGuiKey keysym_to_imgui_key(KeySym ks) {
    switch (ks) {
        case XK_Tab:              return ImGuiKey_Tab;
        case XK_Left:             return ImGuiKey_LeftArrow;
        case XK_Right:            return ImGuiKey_RightArrow;
        case XK_Up:               return ImGuiKey_UpArrow;
        case XK_Down:             return ImGuiKey_DownArrow;
        case XK_Page_Up:          return ImGuiKey_PageUp;
        case XK_Page_Down:        return ImGuiKey_PageDown;
        case XK_Home:             return ImGuiKey_Home;
        case XK_End:              return ImGuiKey_End;
        case XK_Insert:           return ImGuiKey_Insert;
        case XK_Delete:           return ImGuiKey_Delete;
        case XK_BackSpace:        return ImGuiKey_Backspace;
        case XK_space:            return ImGuiKey_Space;
        case XK_Return:           return ImGuiKey_Enter;
        case XK_KP_Enter:         return ImGuiKey_KeypadEnter;
        case XK_Escape:           return ImGuiKey_Escape;
        case XK_Control_L:        return ImGuiKey_LeftCtrl;
        case XK_Control_R:        return ImGuiKey_RightCtrl;
        case XK_Shift_L:          return ImGuiKey_LeftShift;
        case XK_Shift_R:          return ImGuiKey_RightShift;
        case XK_Alt_L:            return ImGuiKey_LeftAlt;
        case XK_Alt_R:            return ImGuiKey_RightAlt;
        case XK_Super_L:          return ImGuiKey_LeftSuper;
        case XK_Super_R:          return ImGuiKey_RightSuper;
        case XK_Menu:             return ImGuiKey_Menu;
        case XK_apostrophe:       return ImGuiKey_Apostrophe;
        case XK_comma:            return ImGuiKey_Comma;
        case XK_minus:            return ImGuiKey_Minus;
        case XK_period:           return ImGuiKey_Period;
        case XK_slash:            return ImGuiKey_Slash;
        case XK_semicolon:        return ImGuiKey_Semicolon;
        case XK_equal:            return ImGuiKey_Equal;
        case XK_bracketleft:      return ImGuiKey_LeftBracket;
        case XK_backslash:        return ImGuiKey_Backslash;
        case XK_bracketright:     return ImGuiKey_RightBracket;
        case XK_grave:            return ImGuiKey_GraveAccent;
        case XK_Caps_Lock:        return ImGuiKey_CapsLock;
        case XK_Scroll_Lock:      return ImGuiKey_ScrollLock;
        case XK_Num_Lock:         return ImGuiKey_NumLock;
        case XK_Print:            return ImGuiKey_PrintScreen;
        case XK_Pause:            return ImGuiKey_Pause;
        default: break;
    }
    if (ks >= XK_0 && ks <= XK_9) return static_cast<ImGuiKey>(ImGuiKey_0 + (ks - XK_0));
    if (ks >= XK_A && ks <= XK_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (ks - XK_A));
    if (ks >= XK_a && ks <= XK_z) return static_cast<ImGuiKey>(ImGuiKey_A + (ks - XK_a));
    if (ks >= XK_F1 && ks <= XK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + (ks - XK_F1));
    if (ks >= XK_KP_0 && ks <= XK_KP_9) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (ks - XK_KP_0));
    return ImGuiKey_None;
}

// ── Cursor setup ───────────────────────────────────────────────

Cursor create_x_cursor(Display* dpy, unsigned shape) {
    return XCreateFontCursor(dpy, shape);
}

void init_cursors(X11BackendContext& c) {
    c.cursors[ImGuiMouseCursor_Arrow]      = create_x_cursor(c.dpy, XC_left_ptr);
    c.cursors[ImGuiMouseCursor_TextInput]  = create_x_cursor(c.dpy, XC_xterm);
    c.cursors[ImGuiMouseCursor_ResizeAll]  = create_x_cursor(c.dpy, XC_fleur);
    c.cursors[ImGuiMouseCursor_ResizeNS]   = create_x_cursor(c.dpy, XC_sb_v_double_arrow);
    c.cursors[ImGuiMouseCursor_ResizeEW]   = create_x_cursor(c.dpy, XC_sb_h_double_arrow);
    c.cursors[ImGuiMouseCursor_ResizeNESW] = create_x_cursor(c.dpy, XC_bottom_left_corner);
    c.cursors[ImGuiMouseCursor_ResizeNWSE] = create_x_cursor(c.dpy, XC_bottom_right_corner);
    c.cursors[ImGuiMouseCursor_Hand]       = create_x_cursor(c.dpy, XC_hand2);
    c.cursors[ImGuiMouseCursor_NotAllowed] = create_x_cursor(c.dpy, XC_X_cursor);
}

void free_cursors(X11BackendContext& c) {
    for (auto& cur : c.cursors) {
        if (cur) {
            XFreeCursor(c.dpy, cur);
            cur = None;
        }
    }
}

void update_cursor(X11BackendContext& c) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) return;

    ImGuiMouseCursor wanted = ImGui::GetMouseCursor();
    Cursor next = None;
    if (wanted == ImGuiMouseCursor_None || io.MouseDrawCursor) {
        // Hide: assign an invisible cursor. Creating one is
        // cheap but noisy; for the first cut keep the arrow.
        next = c.cursors[ImGuiMouseCursor_Arrow];
    } else if (wanted >= 0 && wanted < ImGuiMouseCursor_COUNT) {
        next = c.cursors[wanted];
        if (!next) next = c.cursors[ImGuiMouseCursor_Arrow];
    } else {
        next = c.cursors[ImGuiMouseCursor_Arrow];
    }
    if (next && next != c.current_cursor) {
        XDefineCursor(c.dpy, c.win, next);
        c.current_cursor = next;
    }
}

// ── Clipboard ──────────────────────────────────────────────────
//
// X11 clipboard is an async request/response protocol. A complete
// paste implementation would:
//   1. XConvertSelection(CLIPBOARD, UTF8_STRING, ...)
//   2. Wait for a SelectionNotify event on our window
//   3. Read the property with XGetWindowProperty
// For this first cut we store our own clipboard text in the
// backend struct and serve paste requests synchronously. A
// TODO to wire full async paste is tracked in the header.

const char* get_clipboard_text_fn(ImGuiContext* /*gctx*/) {
    auto& c = ctx();
    return c.last_pasted_text.c_str();
}

void set_clipboard_text_fn(ImGuiContext* /*gctx*/, const char* text) {
    auto& c = ctx();
    c.pending_clipboard_text = text ? text : "";
    c.last_pasted_text = c.pending_clipboard_text;  // same-process loop
    XSetSelectionOwner(c.dpy, c.atom_clipboard, c.win, CurrentTime);
    XFlush(c.dpy);
}

// ── Init / shutdown ────────────────────────────────────────────

}  // anonymous namespace

bool imgui_impl_x11_init(Display* dpy, Window win) {
    if (!dpy || !win) return false;

    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendPlatformUserData == nullptr &&
              "An X11 backend is already bound");

    auto* c = new X11BackendContext;
    c->dpy = dpy;
    c->win = win;
    c->atom_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    c->atom_utf8      = XInternAtom(dpy, "UTF8_STRING", False);
    c->atom_targets   = XInternAtom(dpy, "TARGETS", False);
    c->last_frame_time = std::chrono::steady_clock::now();
    init_cursors(*c);

    io.BackendPlatformUserData = c;
    io.BackendPlatformName = "imgui_impl_x11 (in-house)";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

    auto& pio = ImGui::GetPlatformIO();
    pio.Platform_GetClipboardTextFn = get_clipboard_text_fn;
    pio.Platform_SetClipboardTextFn = set_clipboard_text_fn;
    return true;
}

void imgui_impl_x11_shutdown() {
    ImGuiIO& io = ImGui::GetIO();
    auto* c = static_cast<X11BackendContext*>(io.BackendPlatformUserData);
    if (!c) return;
    free_cursors(*c);
    io.BackendPlatformUserData = nullptr;
    io.BackendPlatformName = nullptr;
    delete c;
}

bool imgui_impl_x11_process_event(const XEvent& ev) {
    ImGuiIO& io = ImGui::GetIO();
    auto& c = ctx();

    switch (ev.type) {
        case MotionNotify:
            io.AddMousePosEvent(static_cast<float>(ev.xmotion.x),
                                static_cast<float>(ev.xmotion.y));
            return true;
        case ButtonPress:
        case ButtonRelease: {
            const bool down = (ev.type == ButtonPress);
            // X11 button codes: 1=left, 2=middle, 3=right,
            // 4/5=wheel up/down, 6/7=wheel left/right.
            switch (ev.xbutton.button) {
                case Button1: io.AddMouseButtonEvent(0, down); return true;
                case Button2: io.AddMouseButtonEvent(2, down); return true;
                case Button3: io.AddMouseButtonEvent(1, down); return true;
                case Button4: if (down) io.AddMouseWheelEvent(0, +1.0f); return true;
                case Button5: if (down) io.AddMouseWheelEvent(0, -1.0f); return true;
                case 6:       if (down) io.AddMouseWheelEvent(-1.0f, 0); return true;
                case 7:       if (down) io.AddMouseWheelEvent(+1.0f, 0); return true;
                default: break;
            }
            return false;
        }
        case KeyPress:
        case KeyRelease: {
            const bool down = (ev.type == KeyPress);
            KeySym ks = XLookupKeysym(const_cast<XKeyEvent*>(&ev.xkey), 0);
            ImGuiKey k = keysym_to_imgui_key(ks);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, down);

            // Modifier state — derive from the event's own state
            // mask rather than tracking L/R Ctrl etc. separately.
            io.AddKeyEvent(ImGuiMod_Ctrl,  (ev.xkey.state & ControlMask) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (ev.xkey.state & ShiftMask)   != 0);
            io.AddKeyEvent(ImGuiMod_Alt,   (ev.xkey.state & Mod1Mask)    != 0);
            io.AddKeyEvent(ImGuiMod_Super, (ev.xkey.state & Mod4Mask)    != 0);

            if (down) {
                // Text input — synchronous (no XIM / compose yet).
                char buf[32] = {};
                int n = XLookupString(const_cast<XKeyEvent*>(&ev.xkey),
                                      buf, sizeof(buf) - 1, nullptr, nullptr);
                if (n > 0) {
                    buf[n] = '\0';
                    io.AddInputCharactersUTF8(buf);
                }
            }
            return true;
        }
        case FocusIn:
            c.focused = true;
            io.AddFocusEvent(true);
            return true;
        case FocusOut:
            c.focused = false;
            io.AddFocusEvent(false);
            return true;
        case EnterNotify:
            io.AddMousePosEvent(static_cast<float>(ev.xcrossing.x),
                                static_cast<float>(ev.xcrossing.y));
            return true;
        case LeaveNotify:
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            return true;
        default:
            return false;
    }
}

void imgui_impl_x11_new_frame() {
    ImGuiIO& io = ImGui::GetIO();
    auto& c = ctx();

    XWindowAttributes attr;
    if (XGetWindowAttributes(c.dpy, c.win, &attr)) {
        io.DisplaySize = ImVec2(static_cast<float>(attr.width),
                                static_cast<float>(attr.height));
    }

    auto now = std::chrono::steady_clock::now();
    auto dt = std::chrono::duration<float>(now - c.last_frame_time).count();
    c.last_frame_time = now;
    // Clamp to avoid ImGui asserting on huge deltas after hibernation.
    io.DeltaTime = dt > 0.f ? (dt > 1.f ? 1.f : dt) : 1.f / 60.f;

    update_cursor(c);
}

}  // namespace xorio_ui::platform::x11
