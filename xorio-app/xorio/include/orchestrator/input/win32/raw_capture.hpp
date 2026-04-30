/// @file win32_raw_capture.hpp
/// @brief Internal helper for the Win32 input backend: captures
/// raw mouse motion + button + scroll events via
/// RegisterRawInputDevices on a dedicated message-only window
/// thread and dispatches them through user-supplied callbacks.
/// Keyboard capture lives in the LL hook (Win32InputGrab) so
/// capture and swallow share one mechanism — see that file.
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

namespace xorio::orchestrator::input {

class Win32RawCapture {
public:
    /// @brief Mirror of @ref MouseButton — kept inline to
    /// avoid pulling input_backend.hpp into this header.
    enum class Button : std::uint8_t {
        Left   = 1,
        Middle = 2,
        Right  = 3,
    };

    using OnMotionFn = std::function<void(std::int32_t dx, std::int32_t dy)>;
    using OnButtonFn = std::function<void(Button button, bool pressed)>;
    using OnScrollFn = std::function<void(std::int32_t dx, std::int32_t dy)>;

    Win32RawCapture()  = default;
    ~Win32RawCapture() { stop(); }

    Win32RawCapture(const Win32RawCapture&)            = delete;
    Win32RawCapture& operator=(const Win32RawCapture&) = delete;

    /// @brief Spawn the message-pump thread and register the
    /// input devices. Idempotent: a second call while already
    /// running is a no-op.
    void start(OnMotionFn on_motion,
               OnButtonFn on_button,
               OnScrollFn on_scroll);

    /// @brief Suppress raw mouse motion events for the next
    /// @p ms milliseconds. The Win32 cursor inject path calls
    /// this before every SetCursorPos / SendInput so the
    /// touchpad-driver phantom-correction echoes that follow
    /// (Synaptics / ELAN / Precision Touchpad on laptops) get
    /// dropped at the raw layer. Real touchpad input on these
    /// devices is indistinguishable from ghosts at the raw
    /// shape level (both come through as hDevice == 0 + a
    /// relative motion event), so the time window is the only
    /// reliable filter. 100 ms covers the burst tail without
    /// blocking the user's first physical touch after a quiet
    /// pause.
    void arm_warp_swallow(int ms);

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
    OnMotionFn        on_motion_;
    OnButtonFn        on_button_;
    OnScrollFn        on_scroll_;

    /// @brief Suppress raw motion events until this absolute
    /// time (steady_clock ms since epoch). Set by
    /// arm_warp_swallow on every cursor injection from the
    /// Win32 backend so the touchpad-driver phantom-correction
    /// echo can't surface as user input. 0 means no suppression.
    std::atomic<std::int64_t> warp_swallow_until_ms_{0};
};

}  // namespace xorio::orchestrator::input
