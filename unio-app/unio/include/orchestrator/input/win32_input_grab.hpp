/// @file win32_input_grab.hpp
/// @brief Internal helper for the Win32 input backend: installs
/// low-level mouse + keyboard hooks (WH_MOUSE_LL, WH_KEYBOARD_LL)
/// that swallow input events so the dormant peer's local apps
/// can't see clicks / keystrokes the user produces while
/// controlling another peer.
///
/// Scope: just the hook lifecycle (install / uninstall + the
/// dedicated message-pump thread the LL hooks need to live on).
/// Knows nothing about cursor warp / inject — that lives in
/// @ref Win32InputBackend, which composes this helper.
///
/// Threading: each instance owns one thread; the LL hooks are
/// per-thread and require a running message loop on the
/// installing thread.
#pragma once

#include <atomic>
#include <thread>

namespace unio_ui::orchestrator::input {

class Win32InputGrab {
public:
    Win32InputGrab()  = default;
    ~Win32InputGrab() { set_grabbed(false, false); }

    Win32InputGrab(const Win32InputGrab&)            = delete;
    Win32InputGrab& operator=(const Win32InputGrab&) = delete;

    /// @brief Switch the grab state. Pointer and keyboard
    /// hooks toggle independently — a workspace can forward
    /// mouse without keyboard or vice-versa. The thread + LL
    /// hooks are shared; the message-pump thread spins up the
    /// first time either grab activates and tears down once
    /// both are off. Idempotent.
    void set_grabbed(bool pointer_grabbed, bool keyboard_grabbed);

private:
    void run_loop();

    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<bool>    want_pointer_grab_{false};
    std::atomic<bool>    want_keyboard_grab_{false};
    unsigned long        thread_id_ = 0;        ///< DWORD; opaque
};

}  // namespace unio_ui::orchestrator::input
