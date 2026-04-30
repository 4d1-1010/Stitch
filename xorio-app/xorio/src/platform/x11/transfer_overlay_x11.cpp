/// @file transfer_overlay_x11.cpp
/// @brief X11 implementation of the floating file-transfer
/// progress overlay.
///
/// Scope: own a single override-redirect window pinned in the
/// upper-right of the primary monitor (queried via RandR so we
/// land on the user's actual display, not a virtual rect that
/// may span multiple monitors), repaint it ~10 Hz from a
/// dedicated worker thread off the main UI thread, auto-show
/// when the orchestrator's progress fetcher returns at least
/// one in-flight transfer and auto-hide as soon as the set
/// goes empty.
///
/// One Display* per overlay instance — never share with the
/// main ImGui+OpenGL Display, since libxcb mixes badly with
/// concurrent Xlib calls from another thread (xorIO Linux
/// gotcha #1).
///
/// Interaction:
///   * Click outside the X button on a row → drag the window.
///   * Click the X button at the right of the row → fire the
///     orchestrator's cancel handler with the row's transfer
///     id; the row tears down on the next refresh.
///
/// Layout per row (top-down):
///   "Sending → <peer>" / "Receiving from <peer>"   (bold)
///   "<label>   <pct>% (<n>/<m>)"                   (regular)
///   ▓▓▓▓▓▓▓▓░░░░░░░░░░░░  bar
///   ✕                                              (right-aligned)

#include "platform/transfer_overlay.hpp"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrandr.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xorio::platform {

namespace {

constexpr int kWindowWidth   = 360;
constexpr int kPadding       = 12;
constexpr int kRowPitch      = 64;
constexpr int kBarHeight     = 8;
constexpr int kHeaderFontPx  = 13;
constexpr int kDetailFontPx  = 11;
constexpr int kCancelSize    = 18;   ///< X button hit + paint size.
constexpr int kMaxRows       = 6;
constexpr int kRefreshHz     = 10;

/// @brief Per-row hit area for routing left-click events to
/// the cancel handler. Recomputed every paint so the test
/// inside the event loop matches what was last drawn.
struct RowHitArea {
    std::uint64_t transfer_id;
    int           cancel_left;
    int           cancel_top;
    int           cancel_right;
    int           cancel_bottom;
};

/// @brief Anchor the overlay on the primary RandR output so
/// multi-monitor setups don't push it off-screen. Falls back
/// to the default screen rect if RandR returns nothing
/// useful.
void resolve_primary_rect(::Display* dpy, int screen,
                          int& out_x, int& out_y,
                          int& out_w, int& out_h) {
    out_x = 0;
    out_y = 0;
    out_w = DisplayWidth (dpy, screen);
    out_h = DisplayHeight(dpy, screen);

    ::Window root = RootWindow(dpy, screen);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    if (res == nullptr) return;

    RROutput primary = XRRGetOutputPrimary(dpy, root);
    auto place_from_output = [&](RROutput o) -> bool {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, o);
        if (oi == nullptr) return false;
        bool ok = false;
        if (oi->connection == 0 && oi->crtc != 0) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            if (ci != nullptr) {
                if (ci->width > 0 && ci->height > 0) {
                    out_x = ci->x;
                    out_y = ci->y;
                    out_w = ci->width;
                    out_h = ci->height;
                    ok = true;
                }
                XRRFreeCrtcInfo(ci);
            }
        }
        XRRFreeOutputInfo(oi);
        return ok;
    };

    bool placed = (primary != 0) && place_from_output(primary);
    if (!placed) {
        for (int i = 0; i < res->noutput; ++i) {
            if (place_from_output(res->outputs[i])) break;
        }
    }
    XRRFreeScreenResources(res);
}

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

    void set_cancel_handler(CancelFn cancel) override {
        std::lock_guard lk(m_);
        cancel_ = std::move(cancel);
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

        // Primary monitor anchor — RandR-aware so multi-monitor
        // setups don't fling the overlay off-screen.
        int prim_x = 0, prim_y = 0, prim_w = 0, prim_h = 0;
        resolve_primary_rect(dpy, screen, prim_x, prim_y, prim_w, prim_h);
        int win_x = prim_x + prim_w - kWindowWidth - 24;
        int win_y = prim_y + 24;

        XSetWindowAttributes swa{};
        swa.override_redirect = True;
        swa.event_mask        = ExposureMask
                              | StructureNotifyMask
                              | ButtonPressMask
                              | ButtonReleaseMask
                              | PointerMotionMask;
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
            if (hdr_font) XftFontClose(dpy, hdr_font);
            if (det_font) XftFontClose(dpy, det_font);
            XDestroyWindow(dpy, win);
            XCloseDisplay(dpy);
            std::lock_guard lk(m_);
            running_ = false;
            return;
        }

        bool   mapped     = false;
        bool   dragging   = false;
        int    drag_off_x = 0;
        int    drag_off_y = 0;
        std::vector<RowHitArea> hit_areas;
        std::vector<TransferOverlayItem> last_items;

        const auto period = std::chrono::milliseconds(1000 / kRefreshHz);
        while (true) {
            {
                std::unique_lock lk(m_);
                cv_.wait_for(lk, period, [&]{ return stop_req_; });
                if (stop_req_) break;
            }

            // Drain pointer events.
            XEvent ev;
            while (XCheckWindowEvent(
                       dpy, win,
                       ButtonPressMask | ButtonReleaseMask
                         | PointerMotionMask | ExposureMask,
                       &ev)) {
                if (ev.type == ButtonPress
                    && ev.xbutton.button == Button1) {
                    const int cx = ev.xbutton.x;
                    const int cy = ev.xbutton.y;
                    std::uint64_t hit_id = 0;
                    bool hit_cancel = false;
                    for (const auto& h : hit_areas) {
                        if (cx >= h.cancel_left
                            && cx <  h.cancel_right
                            && cy >= h.cancel_top
                            && cy <  h.cancel_bottom) {
                            hit_id = h.transfer_id;
                            hit_cancel = true;
                            break;
                        }
                    }
                    if (hit_cancel) {
                        CancelFn fn;
                        {
                            std::lock_guard lk(m_);
                            fn = cancel_;
                        }
                        if (fn) fn(hit_id);
                    } else {
                        // Begin drag — record offset of click
                        // within the window so motion math is
                        // straightforward.
                        dragging   = true;
                        drag_off_x = ev.xbutton.x_root - win_x;
                        drag_off_y = ev.xbutton.y_root - win_y;
                    }
                } else if (ev.type == ButtonRelease
                           && ev.xbutton.button == Button1) {
                    dragging = false;
                } else if (ev.type == MotionNotify && dragging) {
                    win_x = ev.xmotion.x_root - drag_off_x;
                    win_y = ev.xmotion.y_root - drag_off_y;
                    XMoveWindow(dpy, win, win_x, win_y);
                }
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
                hit_areas.clear();
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

            paint(dpy, win, screen, vis, cmap,
                  hdr_font, det_font, items, new_h, hit_areas);
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
                      int height,
                      std::vector<RowHitArea>& hit_areas) {
        GC gc = XCreateGC(dpy, win, 0, nullptr);

        XColor bg{};
        bg.red   = 0x1B1B;
        bg.green = 0x1F1F;
        bg.blue  = 0x2424;
        bg.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(dpy, cmap, &bg);
        XSetForeground(dpy, gc, bg.pixel);
        XFillRectangle(dpy, win, gc, 0, 0, kWindowWidth, height);

        XColor bar_bg{};
        bar_bg.red = 0x3030; bar_bg.green = 0x3535; bar_bg.blue = 0x3D3D;
        bar_bg.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(dpy, cmap, &bar_bg);

        XColor bar_fg{};
        bar_fg.red = 0xA0A0; bar_fg.green = 0x6060; bar_fg.blue = 0xF0F0;
        bar_fg.flags = DoRed | DoGreen | DoBlue;
        XAllocColor(dpy, cmap, &bar_fg);

        XftDraw* draw = XftDrawCreate(dpy, win, vis, cmap);
        XftColor fg{};
        XRenderColor fg_col{0xEFEF, 0xEFEF, 0xF2F2, 0xFFFF};
        XftColorAllocValue(dpy, vis, cmap, &fg_col, &fg);
        XftColor sub{};
        XRenderColor sub_col{0xA8A8, 0xACAC, 0xB6B6, 0xFFFF};
        XftColorAllocValue(dpy, vis, cmap, &sub_col, &sub);
        XftColor cancel_col{};
        XRenderColor cancel_col_v{0xC8C8, 0x6060, 0x6060, 0xFFFF};
        XftColorAllocValue(dpy, vis, cmap, &cancel_col_v, &cancel_col);

        hit_areas.clear();
        hit_areas.reserve(items.size());

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

            const int bar_y = row_y + kRowPitch - kBarHeight - 8;
            const int bar_w = kWindowWidth - 2 * kPadding - kCancelSize - 8;
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

            // Cancel "✕" button at the right edge of the row,
            // vertically centred against the bar.
            const int cx_left = kWindowWidth - kPadding - kCancelSize;
            const int cx_top  = bar_y + (kBarHeight - kCancelSize) / 2;
            // Two diagonal lines forming an ✕. Drawn directly
            // with XDrawLine — small + crisp at this size.
            XSetForeground(dpy, gc, cancel_col.pixel);
            XDrawLine(dpy, win, gc,
                      cx_left + 3,            cx_top + 3,
                      cx_left + kCancelSize-3, cx_top + kCancelSize-3);
            XDrawLine(dpy, win, gc,
                      cx_left + kCancelSize-3, cx_top + 3,
                      cx_left + 3,            cx_top + kCancelSize-3);

            RowHitArea h;
            h.transfer_id   = it.transfer_id;
            h.cancel_left   = cx_left;
            h.cancel_top    = cx_top;
            h.cancel_right  = cx_left + kCancelSize;
            h.cancel_bottom = cx_top  + kCancelSize;
            hit_areas.push_back(h);
        }

        XftColorFree(dpy, vis, cmap, &fg);
        XftColorFree(dpy, vis, cmap, &sub);
        XftColorFree(dpy, vis, cmap, &cancel_col);
        XftDrawDestroy(draw);
        XFreeGC(dpy, gc);
    }

    std::mutex                  m_;
    std::condition_variable     cv_;
    bool                        running_  = false;
    bool                        stop_req_ = false;
    ProgressFetchFn             fetcher_;
    CancelFn                    cancel_;
    std::thread                 worker_;
};

}  // namespace

std::unique_ptr<ITransferOverlay> make_transfer_overlay() {
    return std::make_unique<TransferOverlayX11>();
}

}  // namespace xorio::platform
