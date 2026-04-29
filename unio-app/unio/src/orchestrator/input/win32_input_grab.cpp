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
/// input without the local desktop reacting to it. The keyboard
/// hook also calls back to the orchestrator with the HID-mapped
/// keystroke for cross-mesh forwarding — keeping capture and
/// swallow in one mechanism avoids the empirical suppression of
/// RawInput keyboard delivery a return-1 hook causes on some
/// Windows setups. SendInput-injected events carry the
/// LLMHF_INJECTED / LLKHF_INJECTED flag and are passed through.

#include "orchestrator/input/win32_input_grab.hpp"

#include "orchestrator/input/keycodes.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <utility>

namespace unio_ui::orchestrator::input {

namespace {

// One-instance-at-a-time hooks: the LL hook callbacks have C
// linkage and no userdata pointer, so we route them through
// TU-local globals. The grab is logically singleton anyway
// (one local user, one OS).
HHOOK            g_mouse_hook    = nullptr;
HHOOK            g_keyboard_hook = nullptr;
Win32InputGrab*  g_grab_owner    = nullptr;

LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if (info != nullptr && (info->flags & LLMHF_INJECTED) == 0) {
            // Swallow buttons + scroll only. Motion events
            // pass through so the OS cursor and RawInput
            // pipeline keep operating: RawInput is what feeds
            // our forwarding to the active peer, and a hook
            // that returns 1 on WM_MOUSEMOVE has been observed
            // to suppress RawInput delivery on some Windows
            // setups, leaving the cursor stuck on the source's
            // edge after a cross. The user's local mouse can
            // still move the (hidden) cursor on this PC, but
            // it stays here — the active peer sees the motion
            // via RawInput-driven forwarding, while clicks
            // and wheel events caught here can never reach
            // local apps.
            switch (wp) {
                case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_XBUTTONDOWN: case WM_XBUTTONUP:
                case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
                case WM_MBUTTONDBLCLK: case WM_XBUTTONDBLCLK:
                case WM_MOUSEWHEEL:    case WM_MOUSEHWHEEL:
                    return 1;
                default:
                    break;
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK keyboard_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (info != nullptr && (info->flags & LLKHF_INJECTED) == 0) {
            // Capture the keystroke for forwarding before
            // swallowing it. The vkCode is the L/R-distinguished
            // VK (VK_LSHIFT / VK_RSHIFT etc.) — exactly what
            // vk_to_hid expects.
            const std::uint32_t hid = vk_to_hid(info->vkCode);
            const bool pressed = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            if (hid != 0 && g_grab_owner != nullptr) {
                g_grab_owner->dispatch_key(hid, pressed);
            }
            return 1;
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

constexpr UINT WM_UNIO_GRAB_SYNC = WM_USER + 1;

}  // namespace

Win32InputGrab::~Win32InputGrab() {
    set_grabbed(false, false);
}

void Win32InputGrab::set_on_key(OnKeyFn on_key) {
    std::lock_guard lk(on_key_m_);
    on_key_ = std::move(on_key);
}

void Win32InputGrab::dispatch_key(std::uint32_t hid, bool pressed) {
    OnKeyFn cb;
    {
        std::lock_guard lk(on_key_m_);
        cb = on_key_;
    }
    if (cb) cb(hid, pressed);
}

void Win32InputGrab::set_grabbed(bool pointer_grabbed,
                                  bool keyboard_grabbed) {
    want_pointer_grab_.store(pointer_grabbed,
                              std::memory_order_release);
    want_keyboard_grab_.store(keyboard_grabbed,
                               std::memory_order_release);

    const bool any_grab = pointer_grabbed || keyboard_grabbed;
    const bool was_running = running_.load(std::memory_order_acquire);

    if (any_grab && !was_running) {
        g_grab_owner = this;
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
        g_grab_owner = nullptr;
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
