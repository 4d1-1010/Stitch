/// @file transfer_overlay_x11.cpp
/// @brief X11 implementation of the floating file-transfer
/// progress overlay.
///
/// Scope: own a single override-redirect window pinned in the
/// upper-right of the primary screen, repaint it ~10 Hz from a
/// dedicated worker thread off the main UI thread, auto-show
/// when the orchestrator's progress fetcher returns at least
/// one in-flight transfer and auto-hide as soon as the set
/// goes empty.
///
/// One Display* per overlay instance — never share with the
/// main ImGui+OpenGL Display, since libxcb mixes badly with
/// concurrent Xlib calls from another thread (UnIO Linux
/// gotcha #1).
///
/// Layout per row (top-down):
///   "Sending → <peer>" / "Receiving from <peer>"   (bold)
///   "<label>   <pct>% (<n>/<m>)"                   (regular)
///   ▓▓▓▓▓▓▓▓░░░░░░░░░░░░  bar
/// Rows are stacked with a fixed pitch; window height grows
/// to the current row count, capped at @c kMaxRows so a
/// pathological burst can't push it off-screen.

#include "platform/transfer_overlay.hpp"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace unio_ui::platform {

namespace {

/// @brief Window geometry constants. Width fits two lines of
/// ~50-char text at 13 px without wrapping; row pitch leaves
/// just enough breathing room for the bar.
constexpr int kWindowWidth   = 360;
constexpr int kPadding       = 12;
constexpr int kRowPitch      = 64;
constexpr int kBarHeight     = 8;
constexpr int kHeaderFontPx  = 13;
constexpr int kDetailFontPx  = 11;
constexpr int kMaxRows       = 6;
constexpr int kRefreshHz     = 10;

/// @brief Apply the EWMH hints that tell the WM "this is a
/// floating notification, do not decorate, keep above". Same
/// recipe as identify_overlay_x11.cpp.
void apply_overlay_hints(::Display* dpy, ::Window win) {
    Atom wm_state    = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom above       = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    Atom skip_t      = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom skip_p      = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom atoms[]     = { above, skip_t, skip_p };
    XChangeProperty(dpy, win, wm_state, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(atoms), 3);

    Atom wm_type     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom type_notif  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    XChangeProperty(dpy, win, wm_type, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&type_notif), 1);
}

/// @brief Resolve "Inter:size=N:weight=bold" then a generic
/// fallback so missing fonts don't black out the overlay.
XftFont* open_font(::Display* dpy, int screen, int px, bool bold) {
    char spec[64];
    std::snprintf(spec, sizeof(spec),
                  "Inter:size=%d%s",
                  px, bold ? ":weight=bold" : "");
    XftFont* f = XftFontOpenName(dpy, screen, spec);
    if (f != nullptr) return f;
    std::snprintf(spec, sizeof(spec),
                  "sans-serif:size=%d%s",
                  px, bold ? ":weight=bold" : "");
    return XftFontOpenName(dpy, screen, spec);
}

class TransferOverlayX11 : public ITransferOverlay {
public:
    ~TransferOverlayX11() override { stop(); }

    void set_progress_fetcher(ProgressFetchFn fetch) override {
        std::lock_guard lk(m_);
        fetcher_ = std::move(fetch);
    }

    bool start() override {
        std::lock_guard lk(m_);
        if (running_) return true;
        running_  = true;
        stop_req_ = false;
        worker_   = std::thread(&TransferOverlayX11::run, this);
        return true;
    }

    void stop() override {
        {
            std::lock_guard lk(m_);
            if (!running_) return;
            stop_req_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard lk(m_);
        running_ = false;
    }

private:
    void run() {
        ::Display* dpy = XOpenDisplay(nullptr);
        if (dpy == nullptr) {
            std::lock_guard lk(m_);
            running_ = false;
            return;
        }
        const int screen = DefaultScreen(dpy);
        ::Window  root   = RootWindow(dpy, screen);
        const int sw     = DisplayWidth (dpy, screen);
        // Anchor the window in the top-right corner with a
        // fixed margin. We don't try to track multi-monitor
        // layouts here — the orchestrator's primary-display
        // probe already biases the user's "main" screen.
        const int win_x  = sw - kWindowWidth - 24;
        const int win_y  = 24;

        XSetWindowAttributes swa{};
        swa.override_redirect = True;
        swa.event_mask        = ExposureMask | StructureNotifyMask;
        swa.background_pixmap = None;

        ::Window win = XCreateWindow(
            dpy, root,
            win_x, win_y, kWindowWidth, kRowPitch + 2 * kPadding,
            0, CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWEventMask | CWBackPixmap, &swa);
        apply_overlay_hints(dpy, win);

        Visual*  vis  = DefaultVisual(dpy, screen);
        Colormap cmap = DefaultColormap(dpy, screen);

        XftFont* hdr_font = open_font(dpy, screen, kHeaderFontPx, true);
        XftFont* det_font = open_font(dpy, screen, kDetailFontPx, false);
        if (hdr_font == nullptr || det_font == nullptr) {
            // Font load failed — degrade gracefully by tearing
            // down rather than painting blank overlays for the
            // rest of the session.
            if (hdr_font) XftFontClose(dpy, hdr_font);
            if (det_font) XftFontClose(dpy, det_font);
            XDestroyWindow(dpy, win);
            XCloseDisplay(dpy);
            std::lock_guard lk(m_);
            running_ = false;
            return;
        }

        bool mapped = false;
        std::vector<TransferOverlayItem> last_items;

        const auto period = std::chrono::milliseconds(1000 / kRefreshHz);
        while (true) {
            {
                std::unique_lock lk(m_);
                cv_.wait_for(lk, period, [&]{ return stop_req_; });
                if (stop_req_) break;
            }

            std::vector<TransferOverlayItem> items;
            {
                std::lock_guard lk(m_);
                if (fetcher_) items = fetcher_();
            }

            if (items.empty()) {
                if (mapped) {
                    XUnmapWindow(dpy, win);
                    XFlush(dpy);
                    mapped = false;
                }
                last_items.clear();
                continue;
            }

            if (static_cast<int>(items.size()) > kMaxRows) {
                items.resize(kMaxRows);
            }
            const int rows  = static_cast<int>(items.size());
            const int new_h = rows * kRowPitch + 2 * kPadding;
            XResizeWindow(dpy, win, kWindowWidth, new_h);

            if (!mapped) {
                XMapRaised(dpy, win);
                XFlush(dpy);
                mapped = true;
            }
            // Drain any pending Expose events; we repaint
            // unconditionally below.
            XEvent ev;
            while (XCheckTypedWindowEvent(dpy, win, Expose, &ev)) {}

            paint(dpy, win, screen, vis, cmap,
                  hdr_font, det_font, items, new_h);
            XFlush(dpy);
            last_items = std::move(items);
        }

        if (mapped) XUnmapWindow(dpy, win);
        XftFontClose(dpy, hdr_font);
        XftFontClose(dpy, det_font);
        XDestroyWindow(dpy, win);
        XSync(dpy, True);
        XCloseDisplay(dpy);
    }

    static void paint(::Display* dpy, ::Window win, int screen,
                      Visual* vis, Colormap cmap,
                      XftFont* hdr_font, XftFont* det_font,
                      const std::vector<TransferOverlayItem>& items,
                      int height) {
        GC gc = XCreateGC(dpy, win, 0, nullptr);

        // Background — UnIO charcoal, matches the rail.
        XColor bg{};
        bg.red   = 0x1B1B;
        bg.green = 0x1F1F;
        bg.blue  = 0x2424;
        bg.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(dpy, cmap, &bg);
        XSetForeground(dpy, gc, bg.pixel);
        XFillRectangle(dpy, win, gc, 0, 0, kWindowWidth, height);

        // Bar background — slightly lighter than the window
        // background so the empty portion of the bar still has
        // a visible track.
        XColor bar_bg{};
        bar_bg.red   = 0x3030;
        bar_bg.green = 0x3535;
        bar_bg.blue  = 0x3D3D;
        bar_bg.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(dpy, cmap, &bar_bg);

        // Bar fill — UnIO lilac.
        XColor bar_fg{};
        bar_fg.red   = 0xA0A0;
        bar_fg.green = 0x6060;
        bar_fg.blue  = 0xF0F0;
        bar_fg.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(dpy, cmap, &bar_fg);

        XftDraw* draw = XftDrawCreate(dpy, win, vis, cmap);
        XftColor fg{};
        XRenderColor fg_col{0xEFEF, 0xEFEF, 0xF2F2, 0xFFFF};
        XftColorAllocValue(dpy, vis, cmap, &fg_col, &fg);
        XftColor sub{};
        XRenderColor sub_col{0xA8A8, 0xACAC, 0xB6B6, 0xFFFF};
        XftColorAllocValue(dpy, vis, cmap, &sub_col, &sub);

        const int y_origin = kPadding;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& it = items[i];
            const int row_y = y_origin + static_cast<int>(i) * kRowPitch;

            std::string header =
                (it.direction == TransferOverlayItem::Direction::Sending
                    ? std::string("Sending → ") : std::string("Receiving from "))
                + it.peer_name;

            XftDrawStringUtf8(
                draw, &fg, hdr_font,
                kPadding, row_y + hdr_font->ascent,
                reinterpret_cast<const FcChar8*>(header.c_str()),
                static_cast<int>(header.size()));

            const int pct = (it.bytes_total == 0) ? 0
                : static_cast<int>((it.bytes_done * 100) / it.bytes_total);
            char detail[160];
            if (it.file_count > 1) {
                std::snprintf(detail, sizeof(detail),
                              "%s  ·  %d%%  (%u/%u files)",
                              it.label.c_str(), pct,
                              it.current_file_idx + 1u, it.file_count);
            } else {
                std::snprintf(detail, sizeof(detail),
                              "%s  ·  %d%%",
                              it.label.c_str(), pct);
            }
            const int detail_y = row_y + hdr_font->ascent
                               + 4 + det_font->ascent;
            XftDrawStringUtf8(
                draw, &sub, det_font,
                kPadding, detail_y,
                reinterpret_cast<const FcChar8*>(detail),
                static_cast<int>(std::strlen(detail)));

            // Progress bar.
            const int bar_y = row_y + kRowPitch - kBarHeight - 8;
            const int bar_w = kWindowWidth - 2 * kPadding;
            XSetForeground(dpy, gc, bar_bg.pixel);
            XFillRectangle(dpy, win, gc,
                           kPadding, bar_y, bar_w, kBarHeight);
            int fill_w = (it.bytes_total == 0)
                ? 0
                : static_cast<int>(
                      (static_cast<std::uint64_t>(bar_w) * it.bytes_done)
                      / it.bytes_total);
            if (fill_w > bar_w) fill_w = bar_w;
            if (fill_w > 0) {
                XSetForeground(dpy, gc, bar_fg.pixel);
                XFillRectangle(dpy, win, gc,
                               kPadding, bar_y, fill_w, kBarHeight);
            }
        }

        XftColorFree(dpy, vis, cmap, &fg);
        XftColorFree(dpy, vis, cmap, &sub);
        XftDrawDestroy(draw);
        XFreeGC(dpy, gc);
    }

    std::mutex                  m_;
    std::condition_variable     cv_;
    bool                        running_  = false;
    bool                        stop_req_ = false;
    ProgressFetchFn             fetcher_;
    std::thread                 worker_;
};

}  // namespace

std::unique_ptr<ITransferOverlay> make_transfer_overlay() {
    return std::make_unique<TransferOverlayX11>();
}

}  // namespace unio_ui::platform
