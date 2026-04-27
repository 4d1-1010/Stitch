/// @file win32_raw_capture.hpp
/// @brief Internal helper for the Win32 input backend: captures
/// raw scroll wheel + keyboard events via RegisterRawInputDevices
/// on a dedicated message-only window thread and dispatches them
/// through user-supplied callbacks.
///
/// Scope: just the RawInput event-pump lifecycle (hidden window,
/// device registration, message loop, WM_INPUT decode). Knows
/// nothing about cursor warp / inject / visibility — that lives
/// in @ref Win32InputBackend, which composes this helper.
///
/// Threading: each instance owns one hidden HWND and one thread;
/// the thread runs the message pump that feeds WM_INPUT.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace unio_ui::orchestrator::input {

class Win32RawCapture {
public:
    using OnScrollFn = std::function<void(std::int32_t dx, std::int32_t dy)>;
    using OnKeyFn    = std::function<void(std::uint32_t scancode, bool pressed)>;

    Win32RawCapture()  = default;
    ~Win32RawCapture() { stop(); }

    Win32RawCapture(const Win32RawCapture&)            = delete;
    Win32RawCapture& operator=(const Win32RawCapture&) = delete;

    /// @brief Spawn the message-pump thread and register the
    /// input devices. Idempotent: a second call while already
    /// running is a no-op.
    void start(OnScrollFn on_scroll, OnKeyFn on_key);

    /// @brief Tear down the message pump + hidden window.
    /// Idempotent. Posts WM_QUIT to wake the pump.
    void stop();

    /// @brief Implementation detail surfaced so the static
    /// WndProc can route WM_INPUT back into the instance. Not
    /// intended for outside callers.
    void on_raw_input_message(void* hrawinput);

private:
    void run_loop();

    std::thread       thread_;
    std::atomic<bool> running_{false};
    unsigned long     thread_id_ = 0;       ///< DWORD; kept opaque
    OnScrollFn        on_scroll_;
    OnKeyFn           on_key_;
};

}  // namespace unio_ui::orchestrator::input
