/// @file x11_raw_capture.hpp
/// @brief Internal helper for the X11 input backend: captures
/// raw scroll wheel + keyboard events via XInput2 on a dedicated
/// thread and dispatches them through user-supplied callbacks.
///
/// Scope: just the event-pump lifecycle (open second Display*,
/// XISelectEvents on root, drain XGenericEvent payloads). Knows
/// nothing about cursor warp / inject / visibility — that lives
/// in @ref X11InputBackend, which composes this helper.
///
/// Threading: each instance owns one Display* connection and one
/// thread; X11 connections aren't thread-safe so this stays
/// disjoint from the inject path's connection.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace unio_ui::orchestrator::input {

class X11RawCapture {
public:
    using OnScrollFn = std::function<void(std::int32_t dx, std::int32_t dy)>;
    using OnKeyFn    = std::function<void(std::uint32_t scancode, bool pressed)>;

    X11RawCapture()  = default;
    ~X11RawCapture() { stop(); }

    X11RawCapture(const X11RawCapture&)            = delete;
    X11RawCapture& operator=(const X11RawCapture&) = delete;

    /// @brief Spawn the capture thread. Idempotent: a second call
    /// while already running is a no-op. Returns false if the X
    /// connection or XInput2 extension couldn't be brought up.
    bool start(OnScrollFn on_scroll, OnKeyFn on_key);

    /// @brief Stop the capture thread + close its X connection.
    /// Idempotent. Safe to call from any thread; uses an
    /// XSendEvent ping to wake the blocking XNextEvent.
    void stop();

private:
    void run_loop();

    // void* avoids leaking <X11/Xlib.h> + <X11/extensions/XInput2.h>
    // through this header — only the .cpp needs them.
    void*             display_ = nullptr;
    int               xi_op_   = -1;
    std::thread       thread_;
    std::atomic<bool> running_{false};
    OnScrollFn        on_scroll_;
    OnKeyFn           on_key_;
};

}  // namespace unio_ui::orchestrator::input
