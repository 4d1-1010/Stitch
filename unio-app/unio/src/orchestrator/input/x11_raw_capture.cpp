/// @file x11_raw_capture.cpp
/// @brief XInput2 event-pump implementation for @ref X11RawCapture.
///
/// Selects RawButton + RawKey events on the root window of a
/// dedicated Display* and forwards them to the orchestrator's
/// scroll / key callbacks. Buttons 4-7 are X11's scroll wheel
/// convention (up / down / left / right). Key codes are stripped
/// of the historical +8 offset so the wire format matches the
/// underlying evdev / HID-derived scancodes.

#include "orchestrator/input/x11_raw_capture.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <cstdio>
#include <utility>

namespace unio_ui::orchestrator::input {

namespace {

inline Display* as_display(void* p) {
    return static_cast<Display*>(p);
}

}  // namespace

bool X11RawCapture::start(OnScrollFn on_scroll, OnKeyFn on_key) {
    if (running_.load(std::memory_order_acquire)) return true;

    on_scroll_ = std::move(on_scroll);
    on_key_    = std::move(on_key);

    // Note: avoid `::`-qualifying X11 calls — DefaultRootWindow
    // and friends are macros, and the leading scope operator
    // breaks the expansion ("::(ScreenOfDisplay(...)->root)").
    Display* d = XOpenDisplay(nullptr);
    if (d == nullptr) {
        std::fprintf(stderr,
                     "x11_raw_capture: XOpenDisplay failed\n");
        return false;
    }
    int xi_op = 0, xi_evt = 0, xi_err = 0;
    if (!XQueryExtension(d, "XInputExtension",
                          &xi_op, &xi_evt, &xi_err)) {
        std::fprintf(stderr,
                     "x11_raw_capture: XInputExtension unavailable — "
                     "raw scroll / key forwarding disabled\n");
        XCloseDisplay(d);
        return false;
    }
    int major = 2, minor = 0;
    if (XIQueryVersion(d, &major, &minor) != Success) {
        std::fprintf(stderr,
                     "x11_raw_capture: XIQueryVersion failed\n");
        XCloseDisplay(d);
        return false;
    }

    unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mask_bits, XI_RawButtonPress);
    XISetMask(mask_bits, XI_RawButtonRelease);
    XISetMask(mask_bits, XI_RawKeyPress);
    XISetMask(mask_bits, XI_RawKeyRelease);
    XIEventMask em;
    em.deviceid = XIAllMasterDevices;
    em.mask_len = sizeof(mask_bits);
    em.mask     = mask_bits;
    XISelectEvents(d, DefaultRootWindow(d), &em, 1);
    XFlush(d);

    display_ = d;
    xi_op_   = xi_op;
    running_.store(true, std::memory_order_release);
    thread_  = std::thread(&X11RawCapture::run_loop, this);
    return true;
}

void X11RawCapture::stop() {
    if (!running_.load(std::memory_order_acquire) && display_ == nullptr) {
        return;
    }
    running_.store(false, std::memory_order_release);

    Display* d = as_display(display_);
    if (d != nullptr) {
        // Wake the blocking XNextEvent in run_loop with a no-op
        // ClientMessage. Without this the thread would block
        // forever waiting for input.
        XClientMessageEvent ev{};
        ev.type         = ClientMessage;
        ev.display      = d;
        ev.window       = DefaultRootWindow(d);
        ev.message_type = XInternAtom(d, "UNIO_RAW_STOP", False);
        ev.format       = 32;
        XSendEvent(d, DefaultRootWindow(d),
                    False, NoEventMask,
                    reinterpret_cast<XEvent*>(&ev));
        XFlush(d);
    }
    if (thread_.joinable()) thread_.join();
    if (d != nullptr) {
        XCloseDisplay(d);
        display_ = nullptr;
    }
    xi_op_     = -1;
    on_scroll_ = {};
    on_key_    = {};
}

void X11RawCapture::run_loop() {
    Display* d = as_display(display_);
    while (running_.load(std::memory_order_acquire)) {
        XEvent e;
        XNextEvent(d, &e);
        if (!running_.load(std::memory_order_acquire)) break;
        if (e.type != GenericEvent) continue;
        if (e.xcookie.extension != xi_op_) continue;
        if (!XGetEventData(d, &e.xcookie)) continue;
        const auto* re = static_cast<XIRawEvent*>(e.xcookie.data);
        switch (e.xcookie.evtype) {
            case XI_RawButtonPress: {
                // Buttons 4-7 are the scroll-wheel convention:
                // 4 = up, 5 = down, 6 = left, 7 = right.
                const int btn = re->detail;
                if (btn >= 4 && btn <= 7 && on_scroll_) {
                    const std::int32_t dx = (btn == 7) - (btn == 6);
                    const std::int32_t dy = (btn == 4) - (btn == 5);
                    on_scroll_(dx, dy);
                }
                break;
            }
            case XI_RawKeyPress:
            case XI_RawKeyRelease: {
                if (on_key_) {
                    // X11 keycodes carry a +8 offset over the
                    // underlying evdev / HID-derived scancodes —
                    // strip it for the wire.
                    const std::uint32_t sc =
                        (re->detail >= 8)
                            ? static_cast<std::uint32_t>(re->detail - 8)
                            : 0;
                    on_key_(sc, e.xcookie.evtype == XI_RawKeyPress);
                }
                break;
            }
            default: break;
        }
        XFreeEventData(d, &e.xcookie);
    }
}

}  // namespace unio_ui::orchestrator::input
