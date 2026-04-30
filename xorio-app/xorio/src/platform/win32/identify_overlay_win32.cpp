/// @file identify_overlay_win32.cpp
/// @brief Win32 implementation of the Identify per-monitor overlay.
///
/// Scope: spawn one borderless WS_POPUP window per monitor,
/// painted via GDI with the lilac brand wash + a big number +
/// machine label centred. A worker thread runs its own message
/// pump for the dwell window so the main UI loop is unaffected;
/// pre-empted on a fresh click.

#include "platform/identify_overlay.hpp"

// NOMINMAX before <windows.h> so the Windows headers don't define
// `min` / `max` as macros — those collide with `std::min` /
// `std::max` used below.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xorio_ui::platform {

namespace {

constexpr wchar_t kWndClass[] = L"xorio_identify_overlay";

/// @brief Stop-flag held by both spawner + worker. shared_ptr
/// keeps the flag alive past the spawner's drop so the worker
/// can self-clean even after the next click replaces us.
using StopFlag = std::shared_ptr<std::atomic<bool>>;

std::mutex g_run_mtx;
StopFlag   g_active_stop_flag;

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

void register_class_once() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = overlay_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    wc.hbrBackground = nullptr;  // we paint inside WM_PAINT.
    RegisterClassExW(&wc);
    registered = true;
}

/// @brief Per-window state stashed in the HWND's user data so the
/// paint handler can read the requested number / label.
struct OverlayState {
    int         number = 0;
    std::wstring label;
};

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<OverlayState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);

            // Lilac wash matching the X11 implementation.
            HBRUSH bg = CreateSolidBrush(RGB(0xA0, 0x50, 0xF0));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            if (st != nullptr) {
                const int width  = rc.right  - rc.left;
                const int height = rc.bottom - rc.top;

                // Big number, ~30 % of the smaller axis. White on
                // lilac for high contrast.
                const int big_h   = std::max(72,
                                             std::min(width, height) * 30 / 100);
                const int small_h = std::max(18, big_h / 5);

                HFONT big_font = CreateFontW(
                    big_h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));

                HGDIOBJ prev = SelectObject(hdc, big_font);
                wchar_t buf[16];
                std::swprintf(buf, 16, L"%d", st->number);
                DrawTextW(hdc, buf, -1, &rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                SelectObject(hdc, prev);
                DeleteObject(big_font);

                if (!st->label.empty()) {
                    HFONT small_font = CreateFontW(
                        small_h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
                    RECT lbl = rc;
                    lbl.top = rc.top + (height + big_h) / 2 + 24;
                    HGDIOBJ p2 = SelectObject(hdc, small_font);
                    DrawTextW(hdc, st->label.c_str(), -1, &lbl,
                              DT_CENTER | DT_TOP | DT_SINGLELINE);
                    SelectObject(hdc, p2);
                    DeleteObject(small_font);
                }
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            // OverlayState is heap-owned by the spawning thread;
            // the thread frees it after the loop exits.
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, w, l);
    }
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

/// @brief Hide every visible Windows taskbar (primary + per-
/// monitor secondaries) for the lifetime of an overlay run.
/// WS_EX_TOPMOST popups don't reliably cover the Win10/11 shell
/// — the only deterministic way is to take it off-screen.
struct TaskbarHider {
    std::vector<HWND> hidden;

    TaskbarHider() {
        // Primary taskbar.
        if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
            if (IsWindowVisible(tray)) {
                ShowWindow(tray, SW_HIDE);
                hidden.push_back(tray);
            }
        }
        // Per-monitor secondary taskbars on multi-monitor setups.
        // EnumWindows is overkill — Shell_SecondaryTrayWnd is the
        // class for every secondary; iterate via FindWindowExW
        // until none remain.
        HWND prev = nullptr;
        while (true) {
            HWND tray = FindWindowExW(nullptr, prev,
                                      L"Shell_SecondaryTrayWnd", nullptr);
            if (tray == nullptr) break;
            if (IsWindowVisible(tray)) {
                ShowWindow(tray, SW_HIDE);
                hidden.push_back(tray);
            }
            prev = tray;
        }
    }

    ~TaskbarHider() {
        for (HWND tray : hidden) ShowWindow(tray, SW_SHOW);
    }
};

void run_overlays(std::vector<IdentifyOverlay> overlays,
                  int duration_ms,
                  StopFlag stop_flag) {
    register_class_once();

    HINSTANCE hinst = GetModuleHandleW(nullptr);

    // Hide the taskbar(s) for the dwell. Restored automatically
    // on scope exit (also on early-return from stop_flag).
    TaskbarHider taskbar_hidden;

    std::vector<HWND>                          windows;
    std::vector<std::unique_ptr<OverlayState>> states;
    windows.reserve(overlays.size());
    states.reserve(overlays.size());

    for (const auto& o : overlays) {
        auto st = std::make_unique<OverlayState>();
        st->number = o.number;
        st->label  = widen(o.label);

        // WS_POPUP + WS_EX_TOPMOST + WS_EX_TOOLWINDOW: borderless,
        // no taskbar entry. WS_EX_LAYERED + the explicit
        // SetWindowPos(HWND_TOPMOST) below force the z-order
        // above the Windows taskbar — a plain WS_EX_TOPMOST popup
        // still renders behind the shell on Win10/11.
        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
              | WS_EX_LAYERED,
            kWndClass, nullptr,
            WS_POPUP,
            o.x, o.y, o.width, o.height,
            nullptr, nullptr, hinst, nullptr);
        if (hwnd == nullptr) continue;

        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(st.get()));

        // Layered windows need an opacity bind before they render.
        // Fully opaque (255) — the lilac wash is what the user sees.
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        // Re-apply HWND_TOPMOST after Show — this is the call the
        // shell honours when raising above its own task strip.
        SetWindowPos(hwnd, HWND_TOPMOST,
                     o.x, o.y, o.width, o.height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        windows.push_back(hwnd);
        states.push_back(std::move(st));
    }

    const auto end = std::chrono::steady_clock::now()
                   + std::chrono::milliseconds(duration_ms);
    MSG msg;
    while (std::chrono::steady_clock::now() < end) {
        if (stop_flag->load()) break;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    for (HWND w : windows) DestroyWindow(w);
}

}  // namespace

void show_identify_overlays(const std::vector<IdentifyOverlay>& overlays,
                            int duration_ms) {
    if (overlays.empty()) return;

    StopFlag flag = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lk(g_run_mtx);
        if (g_active_stop_flag) g_active_stop_flag->store(true);
        g_active_stop_flag = flag;
    }
    std::thread(run_overlays, overlays, duration_ms, flag).detach();
}

}  // namespace xorio_ui::platform
