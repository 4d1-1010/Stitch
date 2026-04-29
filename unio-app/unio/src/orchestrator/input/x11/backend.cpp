/// @file input_x11.cpp
/// @brief X11 implementation of @ref IInputBackend — cursor
/// queries via XQueryPointer, cursor warp via XWarpPointer,
/// mouse + key event synthesis via XTestFake* (libXtst), and
/// cursor visibility via XFixes. Local input capture +
/// suppression is owned by a sibling helper (@ref EvdevCapture)
/// so this TU stays focused on the inject / query side of the
/// interface.
///
/// Threading: this instance owns one Display* used for inject /
/// query / visibility. X11 connections aren't thread-safe; the
/// inject path is single-threaded by virtue of being driven
/// from the control-channel reader. The evdev capture helper
/// owns its own reader thread on /dev/input/event* fds.

#include "orchestrator/input/input_backend.hpp"
#include "orchestrator/input/x11/evdev_capture.hpp"
#include "orchestrator/input/keycodes.hpp"

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
        evdev_capture_.stop();
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
        // Wire scancode is HID. Translate via evdev to the X11
        // keycode (= evdev + 8). Drop unknown HID codes — better
        // than synthesising a random keycode that might fire a
        // different key on the receiver.
        const std::uint32_t evdev = hid_to_evdev(scancode);
        if (evdev == 0) return;
        XTestFakeKeyEvent(display_,
                           static_cast<unsigned int>(evdev + 8),
                           pressed ? True : False, 0);
        XFlush(display_);
    }

    void start_raw_capture(RawInputCallbacks cbs) override {
        EvdevCapture::OnButtonFn evdev_button;
        if (cbs.on_button) {
            evdev_button = [cb = std::move(cbs.on_button)](
                                EvdevCapture::Button b, bool pressed) {
                cb(static_cast<MouseButton>(b), pressed);
            };
        }
        evdev_capture_.start(std::move(cbs.on_motion),
                              std::move(evdev_button),
                              std::move(cbs.on_scroll),
                              std::move(cbs.on_key));
    }

    void stop_raw_capture() override {
        evdev_capture_.stop();
    }

    void set_input_grabbed(bool pointer_grabbed,
                            bool keyboard_grabbed) override {
        // Kernel-level grab via EVIOCGRAB on /dev/input/event*
        // — kernel input goes exclusively to us (the only
        // reader of those fds), so X never sees the events
        // and local apps can't react. Capture and block are
        // the same operation. Same model as the Python tree's
        // LinuxX11Backend.
        evdev_capture_.set_grabbed(pointer_grabbed, keyboard_grabbed);
    }

    bool is_input_grabbed() const override {
        return evdev_capture_.any_grabbed();
    }

private:
    std::mutex      m_;
    Display*        display_           = nullptr;
    Window          root_              = 0;
    bool            xtest_ok_          = false;
    bool            xfixes_ok_         = false;
    /// @brief Tracks whether we currently have the cursor
    /// hidden via XFixesHideCursor. ShowCursor on an already-
    /// shown cursor is a BadMatch on some servers.
    bool            cursor_hidden_     = false;

    EvdevCapture    evdev_capture_;
};

}  // namespace

std::unique_ptr<IInputBackend> make_default_input_backend() {
    return std::make_unique<X11InputBackend>();
}

}  // namespace unio_ui::orchestrator::input
