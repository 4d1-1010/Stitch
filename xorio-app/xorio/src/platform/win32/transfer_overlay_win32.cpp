/// @file transfer_overlay_win32.cpp
/// @brief Win32 implementation of the floating file-transfer
/// progress overlay.
///
/// Scope: own a single layered borderless WS_POPUP pinned in the
/// upper-right of the primary monitor, painted via GDI from a
/// dedicated worker thread. Worker runs its own message pump so
/// the main UI's render cadence is unaffected; auto-shows when
/// the orchestrator's progress fetcher returns at least one
/// in-flight transfer and auto-hides when the set goes empty.
///
/// Interaction:
///   * Click outside the X button → drag (forwards to the
///     standard caption-drag path so the OS handles the drag
///     loop natively).
///   * Click the X button → fire the orchestrator's cancel
///     handler with the row's transfer id.

#include "platform/transfer_overlay.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xorio_ui::platform {

namespace {

constexpr wchar_t kWndClass[]   = L"xorio_transfer_overlay";
constexpr int     kWindowWidth  = 360;
constexpr int     kPadding      = 12;
constexpr int     kRowPitch     = 64;
constexpr int     kBarHeight    = 8;
constexpr int     kHeaderFontPx = 16;
constexpr int     kDetailFontPx = 13;
constexpr int     kCancelSize   = 18;
constexpr int     kMaxRows      = 6;
constexpr int     kRefreshHz    = 10;

constexpr UINT WM_XORIO_REFRESH = WM_APP + 0x12;
constexpr UINT WM_XORIO_STOP    = WM_APP + 0x13;

struct RowHitArea {
    std::uint64_t transfer_id;
    RECT          cancel;
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

struct WindowState {
    std::vector<TransferOverlayItem> items;
    std::vector<RowHitArea>          hit_areas;
    ITransferOverlay::CancelFn       cancel;
    std::mutex                       m;
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
        std::lock_guard lk(st->m);
        snap = st->items;
        st->hit_areas.clear();
        st->hit_areas.reserve(snap.size());
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
        const int bar_w = width - 2 * kPadding - kCancelSize - 8;
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

        // Cancel button — two diagonal lines forming an ✕,
        // right-aligned with the bar centre.
        const int cx_left = width - kPadding - kCancelSize;
        const int cx_top  = bar_y + (kBarHeight - kCancelSize) / 2;
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0xC8, 0x60, 0x60));
        HGDIOBJ prev_pen = SelectObject(mem_dc, pen);
        MoveToEx(mem_dc, cx_left + 3, cx_top + 3, nullptr);
        LineTo  (mem_dc, cx_left + kCancelSize - 3,
                          cx_top  + kCancelSize - 3);
        MoveToEx(mem_dc, cx_left + kCancelSize - 3, cx_top + 3, nullptr);
        LineTo  (mem_dc, cx_left + 3, cx_top + kCancelSize - 3);
        SelectObject(mem_dc, prev_pen);
        DeleteObject(pen);

        RowHitArea h;
        h.transfer_id = it.transfer_id;
        h.cancel = RECT{ cx_left, cx_top,
                          cx_left + kCancelSize,
                          cx_top  + kCancelSize };
        std::lock_guard lk(st->m);
        st->hit_areas.push_back(h);
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
            return 1;
        case WM_LBUTTONDOWN: {
            if (!st) return 0;
            const int cx = GET_X_LPARAM(l);
            const int cy = GET_Y_LPARAM(l);
            std::uint64_t hit_id = 0;
            bool          hit_cancel = false;
            ITransferOverlay::CancelFn fn;
            {
                std::lock_guard lk(st->m);
                for (const auto& h : st->hit_areas) {
                    if (cx >= h.cancel.left
                        && cx <  h.cancel.right
                        && cy >= h.cancel.top
                        && cy <  h.cancel.bottom) {
                        hit_id     = h.transfer_id;
                        hit_cancel = true;
                        break;
                    }
                }
                fn = st->cancel;
            }
            if (hit_cancel) {
                if (fn) fn(hit_id);
                return 0;
            }
            // Outside the cancel button: hand off to the OS
            // window-drag loop. ReleaseCapture + sending
            // WM_NCLBUTTONDOWN with HTCAPTION lets a borderless
            // window be dragged exactly like a titlebar.
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }
        case WM_XORIO_STOP:
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

    void set_cancel_handler(CancelFn cancel) override {
        std::lock_guard lk(m_);
        cancel_ = std::move(cancel);
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
        if (hwnd) PostMessageW(hwnd, WM_XORIO_STOP, 0, 0);
        if (worker_.joinable()) worker_.join();
        std::lock_guard lk(m_);
        running_ = false;
        hwnd_    = nullptr;
    }

private:
    void run() {
        register_class_once();

        // Anchor on the primary monitor's work area so multi-
        // monitor setups don't push the window off-screen.
        RECT prim{0, 0, GetSystemMetrics(SM_CXSCREEN),
                          GetSystemMetrics(SM_CYSCREEN)};
        if (HMONITOR mon = MonitorFromPoint(POINT{0, 0},
                                              MONITOR_DEFAULTTOPRIMARY)) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) prim = mi.rcWork;
        }
        const int win_x = prim.right - kWindowWidth - 24;
        const int win_y = prim.top   + 24;

        WindowState st;
        {
            std::lock_guard lk(m_);
            st.cancel = cancel_;
        }
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
                        std::lock_guard lk(st.m);
                        st.items.clear();
                    }
                } else {
                    if (static_cast<int>(items.size()) > kMaxRows) {
                        items.resize(kMaxRows);
                    }
                    const int rows  = static_cast<int>(items.size());
                    const int new_h = rows * kRowPitch + 2 * kPadding;
                    if (!mapped) {
                        SetWindowPos(hwnd, HWND_TOPMOST,
                                     win_x, win_y, kWindowWidth, new_h,
                                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    } else {
                        // Resize-only after the first show so
                        // we don't keep snapping the user-dragged
                        // position back to win_x / win_y.
                        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0,
                                     kWindowWidth, new_h,
                                     SWP_NOMOVE | SWP_NOACTIVATE);
                    }
                    mapped = true;
                    {
                        std::lock_guard lk(st.m);
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
    CancelFn                    cancel_;
    std::thread                 worker_;
};

}  // namespace

std::unique_ptr<ITransferOverlay> make_transfer_overlay() {
    return std::make_unique<TransferOverlayWin32>();
}

}  // namespace xorio_ui::platform
