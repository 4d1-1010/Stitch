/// @file input_x11.cpp
/// @brief X11 implementation of @ref IInputBackend — cursor
/// queries via XQueryPointer, cursor warp via XWarpPointer,
/// mouse + key event synthesis via XTestFake* (libXtst), and
/// cursor visibility via XFixes. Raw scroll + key capture is
/// owned by a sibling helper (@ref X11RawCapture) so this TU
/// stays focused on the inject / query side of the interface.
///
/// Threading: this instance owns one Display* used for inject /
/// query / visibility. X11 connections aren't thread-safe; the
/// inject path is single-threaded by virtue of being driven
/// from the control-channel reader. The raw-capture helper owns
/// its own connection.

#include "orchestrator/input/input_backend.hpp"
#include "orchestrator/input/x11_raw_capture.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace unio_ui::orchestrator::input {

namespace {

class X11InputBackend final : public IInputBackend {
public:
    bool open() override {
        std::lock_guard lk(m_);
        if (display_ != nullptr) return true;
        display_ = XOpenDisplay(nullptr);
        if (display_ == nullptr) {
            std::fprintf(stderr,
                         "input_x11: XOpenDisplay() returned nullptr\n");
            return false;
        }
        root_ = DefaultRootWindow(display_);

        int evt_base = 0, err_base = 0, major = 0, minor = 0;
        if (!XTestQueryExtension(display_, &evt_base, &err_base,
                                  &major, &minor)) {
            std::fprintf(stderr,
                         "input_x11: XTest extension unavailable — "
                         "mouse injection will be a no-op.\n");
            xtest_ok_ = false;
        } else {
            xtest_ok_ = true;
        }
        int xf_evt_base = 0, xf_err_base = 0;
        xfixes_ok_ = false;
        if (XFixesQueryExtension(display_,
                                  &xf_evt_base, &xf_err_base)) {
            // Negotiate the version — without this libXfixes
            // hasn't seeded its per-display state and the
            // ShowCursor / HideCursor opcodes raise BadMatch
            // (we hit this in the wild on the very first hide
            // call, which then poisoned the X connection so
            // every subsequent warp / inject silently no-op'd).
            // Cursor visibility lives in v4; that's our floor.
            int major = 4, minor = 0;
            if (XFixesQueryVersion(display_, &major, &minor)
                && major >= 4) {
                xfixes_ok_ = true;
            }
        }
        if (!xfixes_ok_) {
            std::fprintf(stderr,
                         "input_x11: XFixes >= 4 unavailable — "
                         "cursor hide will be a no-op.\n");
        }
        return true;
    }

    void close() override {
        raw_capture_.stop();
        std::lock_guard lk(m_);
        if (display_ != nullptr) {
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }

    bool get_cursor_pos(std::int32_t& x, std::int32_t& y) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr) return false;
        Window  root_ret = 0, child_ret = 0;
        int     root_x = 0, root_y = 0, win_x = 0, win_y = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(display_, root_,
                            &root_ret, &child_ret,
                            &root_x, &root_y,
                            &win_x, &win_y, &mask)) {
            return false;
        }
        x = root_x;
        y = root_y;
        return true;
    }

    void inject_mouse_move(std::int32_t x, std::int32_t y) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr) return;
        if (xtest_ok_) {
            // -1 screen → use root's screen. Coordinates are
            // root-relative, matching the global virtual screen.
            XTestFakeMotionEvent(display_, -1, x, y, 0);
        } else {
            // Fallback: warp the pointer. Doesn't synthesise a
            // motion event for grabbed clients, but for the
            // un-grabbed common case it works fine.
            XWarpPointer(display_, None, root_,
                         0, 0, 0, 0, x, y);
        }
        XFlush(display_);
    }

    void inject_mouse_button(MouseButton button, bool pressed) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr || !xtest_ok_) return;
        XTestFakeButtonEvent(display_,
                             static_cast<unsigned int>(button),
                             pressed ? True : False, 0);
        XFlush(display_);
    }

    void inject_mouse_scroll(std::int32_t dx, std::int32_t dy) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr || !xtest_ok_) return;
        // X11 scroll convention: button 4 = up, 5 = down,
        // 6 = left, 7 = right. Each scroll "click" is a
        // press + release of the corresponding button.
        auto click = [&](unsigned int btn) {
            XTestFakeButtonEvent(display_, btn, True,  0);
            XTestFakeButtonEvent(display_, btn, False, 0);
        };
        for (std::int32_t i = 0; i < dy; ++i)  click(4);
        for (std::int32_t i = 0; i < -dy; ++i) click(5);
        for (std::int32_t i = 0; i < dx; ++i)  click(7);
        for (std::int32_t i = 0; i < -dx; ++i) click(6);
        XFlush(display_);
    }

    void set_cursor_visible(bool visible) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr || !xfixes_ok_) return;
        // No-op when state matches — calling XFixesShowCursor on
        // an already-shown cursor (or XFixesHideCursor on an
        // already-hidden one) raises BadMatch on some servers.
        if (visible != cursor_hidden_) return;
        if (visible) XFixesShowCursor(display_, root_);
        else         XFixesHideCursor(display_, root_);
        cursor_hidden_ = !visible;
        XFlush(display_);
    }

    std::uint32_t get_button_mask() override {
        std::lock_guard lk(m_);
        if (display_ == nullptr) return 0;
        Window  root_ret = 0, child_ret = 0;
        int     root_x = 0, root_y = 0, win_x = 0, win_y = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(display_, root_,
                            &root_ret, &child_ret,
                            &root_x, &root_y,
                            &win_x, &win_y, &mask)) {
            return 0;
        }
        std::uint32_t out = 0;
        if (mask & Button1Mask) out |= (1u << 0);
        if (mask & Button2Mask) out |= (1u << 1);
        if (mask & Button3Mask) out |= (1u << 2);
        return out;
    }

    void inject_key(std::uint32_t scancode, bool pressed) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr || !xtest_ok_) return;
        // Wire scancode = X11 keycode - 8 (the historical
        // evdev → core-protocol offset). Add it back to inject.
        const unsigned int keycode = scancode + 8;
        XTestFakeKeyEvent(display_, keycode,
                           pressed ? True : False, 0);
        XFlush(display_);
    }

    void start_raw_capture(RawInputCallbacks cbs) override {
        raw_capture_.start(std::move(cbs.on_scroll),
                            std::move(cbs.on_key));
    }

    void stop_raw_capture() override {
        raw_capture_.stop();
    }

    void set_input_grabbed(bool pointer_grabbed,
                            bool keyboard_grabbed) override {
        std::lock_guard lk(m_);
        if (display_ == nullptr) return;
        // GrabModeAsync on both axes so input keeps flowing to
        // our XInput2 raw listener (other clients simply don't
        // see it). owner_events=False because we don't want
        // pointer events delivered to our own ImGui windows
        // either while in grab mode — the events should land
        // *only* on the forwarder.
        if (pointer_grabbed && !pointer_grabbed_) {
            const unsigned int evt_mask = ButtonPressMask
                                         | ButtonReleaseMask
                                         | PointerMotionMask;
            XGrabPointer(display_, root_, False, evt_mask,
                          GrabModeAsync, GrabModeAsync,
                          None, None, CurrentTime);
            pointer_grabbed_ = true;
        } else if (!pointer_grabbed && pointer_grabbed_) {
            XUngrabPointer(display_, CurrentTime);
            pointer_grabbed_ = false;
        }
        if (keyboard_grabbed && !keyboard_grabbed_) {
            XGrabKeyboard(display_, root_, False,
                           GrabModeAsync, GrabModeAsync,
                           CurrentTime);
            keyboard_grabbed_ = true;
        } else if (!keyboard_grabbed && keyboard_grabbed_) {
            XUngrabKeyboard(display_, CurrentTime);
            keyboard_grabbed_ = false;
        }
        XFlush(display_);
    }

private:
    std::mutex      m_;
    Display*        display_           = nullptr;
    Window          root_              = 0;
    bool            xtest_ok_          = false;
    bool            xfixes_ok_         = false;
    bool            pointer_grabbed_   = false;
    bool            keyboard_grabbed_  = false;
    /// @brief Tracks whether we currently have the cursor
    /// hidden via XFixesHideCursor. ShowCursor on an already-
    /// shown cursor is a BadMatch on some servers.
    bool            cursor_hidden_     = false;

    X11RawCapture   raw_capture_;
};

}  // namespace

std::unique_ptr<IInputBackend> make_default_input_backend() {
    return std::make_unique<X11InputBackend>();
}

}  // namespace unio_ui::orchestrator::input
