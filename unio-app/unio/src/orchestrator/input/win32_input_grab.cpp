/// @file win32_input_grab.cpp
/// @brief Low-level mouse + keyboard hook implementation for
/// @ref Win32InputGrab.
///
/// Spins up a dedicated thread to host WH_MOUSE_LL +
/// WH_KEYBOARD_LL hooks (which require a thread message loop).
/// Pointer and keyboard hooks install / uninstall independently
/// so a workspace's Cursor and Keyboard checkboxes can opt in
/// to one stream without the other. While grabbed, the hooks
/// return non-zero to swallow the event before it reaches other
/// apps — the dormant peer becomes a sink that captures user
/// input without the local desktop reacting to it. SendInput-
/// injected events carry the LLMHF_INJECTED / LLKHF_INJECTED
/// flag and are passed through, so our own warps still work.

#include "orchestrator/input/win32_input_grab.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace unio_ui::orchestrator::input {

namespace {

// One-instance-at-a-time hooks: the LL hook callbacks have C
// linkage and no userdata pointer, so we route them through
// TU-local globals. The grab is logically singleton anyway
// (one local user, one OS).
HHOOK g_mouse_hook    = nullptr;
HHOOK g_keyboard_hook = nullptr;

LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if (info != nullptr && (info->flags & LLMHF_INJECTED) == 0) {
            return 1;  // swallow user-driven events
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK keyboard_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (info != nullptr && (info->flags & LLKHF_INJECTED) == 0) {
            return 1;
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

constexpr UINT WM_UNIO_GRAB_SYNC = WM_USER + 1;

}  // namespace

void Win32InputGrab::set_grabbed(bool pointer_grabbed,
                                  bool keyboard_grabbed) {
    want_pointer_grab_.store(pointer_grabbed,
                              std::memory_order_release);
    want_keyboard_grab_.store(keyboard_grabbed,
                               std::memory_order_release);

    const bool any_grab = pointer_grabbed || keyboard_grabbed;
    const bool was_running = running_.load(std::memory_order_acquire);

    if (any_grab && !was_running) {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&Win32InputGrab::run_loop, this);
    } else if (any_grab && was_running) {
        // Already running — nudge the pump to re-evaluate
        // which hooks should be installed.
        if (thread_id_ != 0) {
            ::PostThreadMessageW(thread_id_,
                                  WM_UNIO_GRAB_SYNC, 0, 0);
        }
    } else if (!any_grab && was_running) {
        running_.store(false, std::memory_order_release);
        if (thread_id_ != 0) {
            ::PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
        }
        if (thread_.joinable()) thread_.join();
        thread_id_ = 0;
    }
}

void Win32InputGrab::run_loop() {
    thread_id_ = ::GetCurrentThreadId();

    auto sync_hooks = [this]() {
        const bool want_p = want_pointer_grab_.load(std::memory_order_acquire);
        const bool want_k = want_keyboard_grab_.load(std::memory_order_acquire);
        if (want_p && g_mouse_hook == nullptr) {
            g_mouse_hook = ::SetWindowsHookExW(
                WH_MOUSE_LL, &mouse_proc,
                ::GetModuleHandleW(nullptr), 0);
        } else if (!want_p && g_mouse_hook != nullptr) {
            ::UnhookWindowsHookEx(g_mouse_hook);
            g_mouse_hook = nullptr;
        }
        if (want_k && g_keyboard_hook == nullptr) {
            g_keyboard_hook = ::SetWindowsHookExW(
                WH_KEYBOARD_LL, &keyboard_proc,
                ::GetModuleHandleW(nullptr), 0);
        } else if (!want_k && g_keyboard_hook != nullptr) {
            ::UnhookWindowsHookEx(g_keyboard_hook);
            g_keyboard_hook = nullptr;
        }
    };
    sync_hooks();

    MSG msg;
    while (running_.load(std::memory_order_acquire)
           && ::GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_UNIO_GRAB_SYNC) {
            sync_hooks();
            continue;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (g_mouse_hook != nullptr) {
        ::UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = nullptr;
    }
    if (g_keyboard_hook != nullptr) {
        ::UnhookWindowsHookEx(g_keyboard_hook);
        g_keyboard_hook = nullptr;
    }
}

}  // namespace unio_ui::orchestrator::input
