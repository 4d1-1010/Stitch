/// @file transfer_overlay_win32.cpp
/// @brief Win32 implementation of the floating file-transfer
/// progress overlay.
///
/// Scope: own a single layered borderless WS_POPUP pinned in the
/// upper-right of the primary monitor, painted via GDI from a
/// dedicated worker thread. The worker runs its own message
/// pump so the main UI's WM_PAINT cadence is unaffected, and
/// polls the orchestrator's progress fetcher at @ref kRefreshHz.
/// Window auto-shows when the fetch returns at least one
/// in-flight transfer and auto-hides when the set goes empty.
///
/// We don't reuse the Identify overlay — Identify is a one-shot
/// dwell; this one is long-lived and must redraw smoothly while
/// transfers run.

#include "platform/transfer_overlay.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace unio_ui::platform {

namespace {

constexpr wchar_t kWndClass[]  = L"unio_transfer_overlay";
constexpr int     kWindowWidth  = 360;
constexpr int     kPadding      = 12;
constexpr int     kRowPitch     = 64;
constexpr int     kBarHeight    = 8;
constexpr int     kHeaderFontPx = 16;
constexpr int     kDetailFontPx = 13;
constexpr int     kMaxRows      = 6;
constexpr int     kRefreshHz    = 10;

constexpr UINT WM_UNIO_REFRESH = WM_APP + 0x12;
constexpr UINT WM_UNIO_STOP    = WM_APP + 0x13;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

struct WindowState {
    std::vector<TransferOverlayItem> items;
    std::mutex                       items_m;
};

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg,
                                 WPARAM w, LPARAM l);

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
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);
    registered = true;
}

void paint_window(HWND hwnd, WindowState* st) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int width  = rc.right  - rc.left;
    const int height = rc.bottom - rc.top;

    // Double-buffer to a memory DC — direct GDI on a layered
    // window flickers visibly during the 10 Hz repaint.
    HDC     mem_dc = CreateCompatibleDC(hdc);
    HBITMAP mem_bm = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ old_bm = SelectObject(mem_dc, mem_bm);

    HBRUSH bg = CreateSolidBrush(RGB(0x1B, 0x1F, 0x24));
    FillRect(mem_dc, &rc, bg);
    DeleteObject(bg);

    HFONT hdr_font = CreateFontW(
        kHeaderFontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT det_font = CreateFontW(
        kDetailFontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    SetBkMode(mem_dc, TRANSPARENT);

    std::vector<TransferOverlayItem> snap;
    {
        std::lock_guard lk(st->items_m);
        snap = st->items;
    }

    for (std::size_t i = 0; i < snap.size(); ++i) {
        const auto& it = snap[i];
        const int row_y = kPadding + static_cast<int>(i) * kRowPitch;

        std::wstring header = (it.direction
                == TransferOverlayItem::Direction::Sending
                    ? std::wstring(L"Sending → ")
                    : std::wstring(L"Receiving from "))
                + widen(it.peer_name);

        RECT hdr_rc{ kPadding, row_y,
                     width - kPadding, row_y + kHeaderFontPx + 6 };
        SetTextColor(mem_dc, RGB(0xEF, 0xEF, 0xF2));
        HGDIOBJ prev_f = SelectObject(mem_dc, hdr_font);
        DrawTextW(mem_dc, header.c_str(), -1, &hdr_rc,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem_dc, prev_f);

        const int pct = (it.bytes_total == 0) ? 0
            : static_cast<int>((it.bytes_done * 100) / it.bytes_total);
        wchar_t detail[256];
        if (it.file_count > 1) {
            std::swprintf(detail, 256,
                          L"%ls  ·  %d%%  (%u/%u files)",
                          widen(it.label).c_str(), pct,
                          it.current_file_idx + 1u, it.file_count);
        } else {
            std::swprintf(detail, 256,
                          L"%ls  ·  %d%%",
                          widen(it.label).c_str(), pct);
        }
        RECT det_rc{ kPadding, row_y + kHeaderFontPx + 6,
                     width - kPadding,
                     row_y + kHeaderFontPx + 6 + kDetailFontPx + 4 };
        SetTextColor(mem_dc, RGB(0xA8, 0xAC, 0xB6));
        HGDIOBJ prev_d = SelectObject(mem_dc, det_font);
        DrawTextW(mem_dc, detail, -1, &det_rc,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(mem_dc, prev_d);

        const int bar_y = row_y + kRowPitch - kBarHeight - 8;
        const int bar_w = width - 2 * kPadding;
        RECT bar_bg_rc{ kPadding, bar_y,
                        kPadding + bar_w, bar_y + kBarHeight };
        HBRUSH bar_bg = CreateSolidBrush(RGB(0x30, 0x35, 0x3D));
        FillRect(mem_dc, &bar_bg_rc, bar_bg);
        DeleteObject(bar_bg);

        int fill_w = (it.bytes_total == 0) ? 0
            : static_cast<int>(
                  (static_cast<unsigned long long>(bar_w) * it.bytes_done)
                  / it.bytes_total);
        if (fill_w > bar_w) fill_w = bar_w;
        if (fill_w > 0) {
            RECT bar_fg_rc{ kPadding, bar_y,
                            kPadding + fill_w, bar_y + kBarHeight };
            HBRUSH bar_fg = CreateSolidBrush(RGB(0xA0, 0x60, 0xF0));
            FillRect(mem_dc, &bar_fg_rc, bar_fg);
            DeleteObject(bar_fg);
        }
    }

    BitBlt(hdc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);

    DeleteObject(hdr_font);
    DeleteObject(det_font);
    SelectObject(mem_dc, old_bm);
    DeleteObject(mem_bm);
    DeleteDC(mem_dc);

    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg,
                                 WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<WindowState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT:
            if (st) paint_window(hwnd, st);
            return 0;
        case WM_ERASEBKGND:
            return 1;  // we paint everything in WM_PAINT.
        case WM_UNIO_STOP:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, w, l);
    }
}

class TransferOverlayWin32 : public ITransferOverlay {
public:
    ~TransferOverlayWin32() override { stop(); }

    void set_progress_fetcher(ProgressFetchFn fetch) override {
        std::lock_guard lk(m_);
        fetcher_ = std::move(fetch);
    }

    bool start() override {
        std::lock_guard lk(m_);
        if (running_) return true;
        running_  = true;
        stop_req_ = false;
        worker_   = std::thread(&TransferOverlayWin32::run, this);
        return true;
    }

    void stop() override {
        HWND hwnd = nullptr;
        {
            std::lock_guard lk(m_);
            if (!running_) return;
            stop_req_ = true;
            hwnd      = hwnd_;
        }
        if (hwnd) PostMessageW(hwnd, WM_UNIO_STOP, 0, 0);
        if (worker_.joinable()) worker_.join();
        std::lock_guard lk(m_);
        running_ = false;
        hwnd_    = nullptr;
    }

private:
    void run() {
        register_class_once();

        const int sw    = GetSystemMetrics(SM_CXSCREEN);
        const int win_x = sw - kWindowWidth - 24;
        const int win_y = 24;

        WindowState st;
        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
              | WS_EX_LAYERED,
            kWndClass, nullptr,
            WS_POPUP,
            win_x, win_y, kWindowWidth, kRowPitch + 2 * kPadding,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (hwnd == nullptr) {
            std::lock_guard lk(m_);
            running_ = false;
            return;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(&st));
        SetLayeredWindowAttributes(hwnd, 0, 230, LWA_ALPHA);
        {
            std::lock_guard lk(m_);
            hwnd_ = hwnd;
        }

        bool mapped = false;
        const auto period = std::chrono::milliseconds(1000 / kRefreshHz);
        auto next_tick = std::chrono::steady_clock::now() + period;

        MSG msg;
        for (;;) {
            // Pump pending messages without blocking — we own
            // the cadence below.
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) goto done;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            {
                std::lock_guard lk(m_);
                if (stop_req_) break;
            }

            if (std::chrono::steady_clock::now() >= next_tick) {
                next_tick += period;

                std::vector<TransferOverlayItem> items;
                {
                    std::lock_guard lk(m_);
                    if (fetcher_) items = fetcher_();
                }

                if (items.empty()) {
                    if (mapped) {
                        ShowWindow(hwnd, SW_HIDE);
                        mapped = false;
                    }
                    {
                        std::lock_guard lk(st.items_m);
                        st.items.clear();
                    }
                } else {
                    if (static_cast<int>(items.size()) > kMaxRows) {
                        items.resize(kMaxRows);
                    }
                    const int rows  = static_cast<int>(items.size());
                    const int new_h = rows * kRowPitch + 2 * kPadding;
                    SetWindowPos(hwnd, HWND_TOPMOST,
                                 win_x, win_y, kWindowWidth, new_h,
                                 SWP_NOACTIVATE
                                  | (mapped ? 0u : SWP_SHOWWINDOW));
                    mapped = true;
                    {
                        std::lock_guard lk(st.items_m);
                        st.items = std::move(items);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        if (IsWindow(hwnd)) DestroyWindow(hwnd);

    done:
        std::lock_guard lk(m_);
        hwnd_ = nullptr;
    }

    std::mutex                  m_;
    std::condition_variable     cv_;
    bool                        running_  = false;
    bool                        stop_req_ = false;
    HWND                        hwnd_     = nullptr;
    ProgressFetchFn             fetcher_;
    std::thread                 worker_;
};

}  // namespace

std::unique_ptr<ITransferOverlay> make_transfer_overlay() {
    return std::make_unique<TransferOverlayWin32>();
}

}  // namespace unio_ui::platform
