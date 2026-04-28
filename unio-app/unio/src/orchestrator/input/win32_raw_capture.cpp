/// @file win32_raw_capture.cpp
/// @brief RawInput event-pump implementation for
/// @ref Win32RawCapture.
///
/// Spins up a hidden message-only window on a dedicated thread,
/// installs RIDEV_INPUTSINK for mouse + keyboard, and decodes
/// WM_INPUT into scroll-detent + scancode events for the
/// orchestrator. The PS/2 E0 prefix is folded into bit 8 of the
/// emitted scancode so the wire format matches the Linux side's
/// evdev-derived codes.

#include "orchestrator/input/win32_raw_capture.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <utility>
#include <vector>

namespace unio_ui::orchestrator::input {

namespace {

constexpr wchar_t kWndClassName[] = L"unio_raw_input";

LRESULT CALLBACK raw_wnd_proc(HWND hwnd, UINT msg,
                               WPARAM wp, LPARAM lp) {
    if (msg == WM_INPUT) {
        auto* self = reinterpret_cast<Win32RawCapture*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr) {
            self->on_raw_input_message(reinterpret_cast<void*>(lp));
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void Win32RawCapture::start(OnMotionFn on_motion,
                              OnScrollFn on_scroll,
                              OnKeyFn    on_key) {
    if (running_.load(std::memory_order_acquire)) return;
    on_motion_ = std::move(on_motion);
    on_scroll_ = std::move(on_scroll);
    on_key_    = std::move(on_key);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&Win32RawCapture::run_loop, this);
}

void Win32RawCapture::arm_warp_swallow(int ms) {
    if (ms <= 0) return;
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    const auto deadline =
        now + static_cast<std::int64_t>(ms);
    auto cur = warp_swallow_until_ms_.load(std::memory_order_acquire);
    while (cur < deadline) {
        if (warp_swallow_until_ms_.compare_exchange_weak(
                cur, deadline,
                std::memory_order_release,
                std::memory_order_acquire)) {
            break;
        }
    }
}

void Win32RawCapture::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (thread_id_ != 0) {
        ::PostThreadMessageW(thread_id_, WM_QUIT, 0, 0);
    }
    if (thread_.joinable()) thread_.join();
    thread_id_ = 0;
    on_motion_ = {};
    on_scroll_ = {};
    on_key_    = {};
}

void Win32RawCapture::run_loop() {
    thread_id_ = ::GetCurrentThreadId();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &raw_wnd_proc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kWndClassName;
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(
        0, kWndClassName, L"", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr,
        wc.hInstance, this);
    if (hwnd == nullptr) {
        running_.store(false, std::memory_order_release);
        return;
    }
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(this));

    RAWINPUTDEVICE rid[2]{};
    rid[0].usUsagePage = 0x01;       // Generic Desktop
    rid[0].usUsage     = 0x02;       // Mouse
    rid[0].dwFlags     = RIDEV_INPUTSINK;
    rid[0].hwndTarget  = hwnd;
    rid[1].usUsagePage = 0x01;
    rid[1].usUsage     = 0x06;       // Keyboard
    rid[1].dwFlags     = RIDEV_INPUTSINK;
    rid[1].hwndTarget  = hwnd;
    ::RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));

    MSG msg;
    while (running_.load(std::memory_order_acquire)
           && ::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    rid[0].dwFlags    = RIDEV_REMOVE;
    rid[0].hwndTarget = nullptr;
    rid[1].dwFlags    = RIDEV_REMOVE;
    rid[1].hwndTarget = nullptr;
    ::RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE));

    ::DestroyWindow(hwnd);
    ::UnregisterClassW(kWndClassName, wc.hInstance);
}

void Win32RawCapture::on_raw_input_message(void* hrawinput_v) {
    HRAWINPUT h = reinterpret_cast<HRAWINPUT>(hrawinput_v);
    UINT size = 0;
    if (::GetRawInputData(h, RID_INPUT, nullptr, &size,
                           sizeof(RAWINPUTHEADER)) != 0) {
        return;
    }
    std::vector<BYTE> buf(size);
    if (::GetRawInputData(h, RID_INPUT, buf.data(), &size,
                           sizeof(RAWINPUTHEADER)) != size) {
        return;
    }
    const auto* ri = reinterpret_cast<RAWINPUT*>(buf.data());
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        const auto& mouse = ri->data.mouse;
        // Modern precision-touchpad drivers report finger
        // motion as RawInput events with hDevice == 0 and
        // flags == 0 (relative) — exactly the same shape as
        // the touchpad's own phantom-correction echo events
        // that follow every SetCursorPos / SendInput warp.
        // hDevice / flags can't tell them apart, so we filter
        // by *time* instead: the inject path arms a swallow
        // window before every cursor write, and we drop raw
        // motion events that arrive inside it.
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();
        const auto deadline =
            warp_swallow_until_ms_.load(std::memory_order_acquire);
        const bool in_swallow_window = now_ms < deadline;
        if (!in_swallow_window
            && (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0
            && (mouse.lLastX != 0 || mouse.lLastY != 0)
            && on_motion_) {
            on_motion_(static_cast<std::int32_t>(mouse.lLastX),
                        static_cast<std::int32_t>(mouse.lLastY));
        }
        const USHORT flags = mouse.usButtonFlags;
        if ((flags & RI_MOUSE_WHEEL) && on_scroll_) {
            const std::int16_t delta = static_cast<std::int16_t>(
                mouse.usButtonData);
            on_scroll_(0, delta / WHEEL_DELTA);
        }
        if ((flags & RI_MOUSE_HWHEEL) && on_scroll_) {
            const std::int16_t delta = static_cast<std::int16_t>(
                mouse.usButtonData);
            on_scroll_(delta / WHEEL_DELTA, 0);
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const auto& kb = ri->data.keyboard;
        // Fold the E0 prefix into bit 8 so the wire format
        // matches the Linux side's evdev-derived codes.
        std::uint32_t sc = kb.MakeCode;
        if (kb.Flags & RI_KEY_E0) sc |= 0x100u;
        const bool pressed = (kb.Flags & RI_KEY_BREAK) == 0;
        if (on_key_) on_key_(sc, pressed);
    }
}

}  // namespace unio_ui::orchestrator::input
