/// @file win32_input_grab.hpp
/// @brief Internal helper for the Win32 input backend: installs
/// low-level mouse + keyboard hooks (WH_MOUSE_LL, WH_KEYBOARD_LL).
/// The keyboard hook also captures key events for forwarding —
/// keeping capture and swallow in one mechanism mirrors the
/// Python tree's design and avoids the LL-hook-vs-RawInput
/// suppression issue (a hook that returns 1 silently kills the
/// RawInput keyboard stream on some setups).
///
/// Scope: hook lifecycle (install / uninstall + the dedicated
/// message-pump thread the LL hooks need to live on) + the
/// keyboard capture callback. Knows nothing about cursor warp
/// / inject — that lives in @ref Win32InputBackend, which
/// composes this helper.
///
/// Threading: each instance owns one thread; the LL hooks are
/// per-thread and require a running message loop on the
/// installing thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace unio_ui::orchestrator::input {

class Win32InputGrab {
public:
    /// @c hid is a USB HID Usage ID (Keyboard/Keypad page 0x07);
    /// see @ref keycodes.hpp. The hook translates VK → HID at
    /// the boundary so the wire format stays uniform.
    using OnKeyFn =
        std::function<void(std::uint32_t hid, bool pressed)>;

    Win32InputGrab()  = default;
    ~Win32InputGrab();

    Win32InputGrab(const Win32InputGrab&)            = delete;
    Win32InputGrab& operator=(const Win32InputGrab&) = delete;

    /// @brief Register the key capture callback. Called once at
    /// backend start; the callback fires from the hook thread
    /// for every non-injected keystroke regardless of grab
    /// state — the orchestrator's forwarding gate ignores
    /// captured keys when not dormant.
    void set_on_key(OnKeyFn on_key);

    /// @brief Switch the grab state. Pointer and keyboard
    /// hooks toggle independently — a workspace can forward
    /// mouse without keyboard or vice-versa. The thread + LL
    /// hooks are shared; the message-pump thread spins up the
    /// first time either grab activates and tears down once
    /// both are off. Idempotent.
    void set_grabbed(bool pointer_grabbed, bool keyboard_grabbed);

    /// @brief Public bridge from the C-linkage keyboard hook
    /// proc into our @c on_key_ callback. Not part of the
    /// stable interface — only the LL hook should call it.
    void dispatch_key(std::uint32_t hid, bool pressed);

private:
    void run_loop();

    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<bool>    want_pointer_grab_{false};
    std::atomic<bool>    want_keyboard_grab_{false};
    unsigned long        thread_id_ = 0;        ///< DWORD; opaque

    /// @brief Guarded so set_on_key from the orchestrator
    /// thread doesn't race the hook callback's read.
    std::mutex           on_key_m_;
    OnKeyFn              on_key_;
};

}  // namespace unio_ui::orchestrator::input
