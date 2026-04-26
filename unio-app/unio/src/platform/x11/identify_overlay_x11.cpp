/// @file identify_overlay_x11.cpp
/// @brief X11 implementation of the Identify per-monitor overlay.
///
/// Scope: spawn one borderless override-redirect window per
/// monitor, paint it with the machine accent + a big number +
/// small label, tear them all down once the dwell elapses. Runs
/// on its own worker thread with its own X display connection so
/// the main UI thread's Xlib calls are not interleaved.
///
/// Text is drawn via libXft so the digit reads at any monitor
/// size without falling back to the X core bitmap fonts (which
/// are unreliable on modern desktops).

#include "platform/identify_overlay.hpp"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace unio_ui::platform {

namespace {

/// @brief Stop-flag held by both the spawning thread and the
/// worker. shared_ptr lets the worker survive even after the
/// spawner has dropped its reference (next click) — the worker
/// reads its own copy of the flag and exits cleanly when set.
using StopFlag = std::shared_ptr<std::atomic<bool>>;

std::mutex g_run_mtx;
StopFlag   g_active_stop_flag;

/// @brief Configure @p win as a non-decorated fullscreen overlay
/// pinned above the user's regular windows. Uses the standard
/// EWMH / Motif-WM hints so every contemporary window manager
/// honours the request.
void apply_overlay_hints(::Display* dpy, ::Window win) {
    Atom atom_wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom atom_above    = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    Atom atom_skip_t   = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom atom_skip_p   = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom atoms[] = { atom_above, atom_skip_t, atom_skip_p };
    XChangeProperty(dpy, win, atom_wm_state, XA_ATOM, 32,
                    PropModeReplace,
                    reinterpret_cast<unsigned char*>(atoms), 3);

    Atom atom_type     = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom atom_type_dlg = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
    XChangeProperty(dpy, win, atom_type, XA_ATOM, 32,
                    PropModeReplace,
                    reinterpret_cast<unsigned char*>(&atom_type_dlg), 1);
}

/// @brief Paint one overlay's contents — solid lilac wash, big
/// digit centred, machine_id label below it.
void paint_overlay(::Display* dpy, ::Window win, int screen,
                   int width, int height,
                   const IdentifyOverlay& o) {
    Visual* vis  = DefaultVisual(dpy, screen);
    Colormap cmap = DefaultColormap(dpy, screen);

    // Solid background — UnIO's lilac brand accent so every
    // overlay reads as "the app made this", regardless of the
    // user's desktop wallpaper.
    GC gc = XCreateGC(dpy, win, 0, nullptr);
    XColor bg{};
    bg.red   = 0xA050;
    bg.green = 0x50F0;
    bg.blue  = 0xF0F0;
    bg.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, cmap, &bg);
    XSetForeground(dpy, gc, bg.pixel);
    XFillRectangle(dpy, win, gc, 0, 0, width, height);

    // Big number — Xft scales the font outline, so any monitor
    // size renders crisply. Font size scaled to ~30 % of the
    // shorter axis so a 4K and a 1080p panel both look right.
    const int big_px   = std::max(72, std::min(width, height) * 30 / 100);
    const int small_px = std::max(18, big_px / 5);

    char big_font_spec[64];
    char small_font_spec[64];
    std::snprintf(big_font_spec,   sizeof(big_font_spec),
                  "Inter:size=%d:weight=bold", big_px);
    std::snprintf(small_font_spec, sizeof(small_font_spec),
                  "Inter:size=%d", small_px);

    XftFont* big_font   = XftFontOpenName(dpy, screen, big_font_spec);
    XftFont* small_font = XftFontOpenName(dpy, screen, small_font_spec);
    if (big_font == nullptr) {
        // Inter not installed (rare on Linux desktops, but possible
        // on minimal containers) — fall back to the generic family.
        big_font = XftFontOpenName(dpy, screen, "sans-serif:size=120:weight=bold");
    }
    if (small_font == nullptr) {
        small_font = XftFontOpenName(dpy, screen, "sans-serif:size=24");
    }
    if (big_font == nullptr || small_font == nullptr) {
        XFreeGC(dpy, gc);
        return;
    }

    XftDraw* draw = XftDrawCreate(dpy, win, vis, cmap);

    XftColor fg{};
    XRenderColor fg_col{0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    XftColorAllocValue(dpy, vis, cmap, &fg_col, &fg);

    char num_buf[16];
    std::snprintf(num_buf, sizeof(num_buf), "%d", o.number);
    const std::size_t num_len = std::strlen(num_buf);

    XGlyphInfo big_extents{};
    XftTextExtents8(dpy, big_font,
                    reinterpret_cast<const FcChar8*>(num_buf),
                    static_cast<int>(num_len), &big_extents);
    const int big_x = (width  - big_extents.width)  / 2 + big_extents.x;
    const int big_y = (height - big_extents.height) / 2 + big_extents.y;
    XftDrawString8(draw, &fg, big_font, big_x, big_y,
                   reinterpret_cast<const FcChar8*>(num_buf),
                   static_cast<int>(num_len));

    if (!o.label.empty()) {
        XGlyphInfo lbl_extents{};
        XftTextExtents8(dpy, small_font,
                        reinterpret_cast<const FcChar8*>(o.label.c_str()),
                        static_cast<int>(o.label.size()),
                        &lbl_extents);
        const int lbl_x = (width  - lbl_extents.width)  / 2 + lbl_extents.x;
        const int lbl_y = big_y + big_extents.height / 2 + lbl_extents.height + 16;
        XftDrawString8(draw, &fg, small_font, lbl_x, lbl_y,
                       reinterpret_cast<const FcChar8*>(o.label.c_str()),
                       static_cast<int>(o.label.size()));
    }

    XftColorFree(dpy, vis, cmap, &fg);
    XftDrawDestroy(draw);
    XftFontClose(dpy, big_font);
    XftFontClose(dpy, small_font);
    XFreeGC(dpy, gc);
}

void run_overlays(std::vector<IdentifyOverlay> overlays,
                  int duration_ms,
                  StopFlag stop_flag) {
    ::Display* dpy = XOpenDisplay(nullptr);
    if (dpy == nullptr) return;

    const int screen = DefaultScreen(dpy);
    ::Window  root   = RootWindow(dpy, screen);

    XSetWindowAttributes swa{};
    swa.override_redirect = True;          // bypass the WM entirely
    swa.event_mask        = ExposureMask;
    swa.background_pixmap = None;

    std::vector<::Window> windows;
    windows.reserve(overlays.size());
    for (const auto& o : overlays) {
        ::Window w = XCreateWindow(
            dpy, root,
            o.x, o.y, o.width, o.height,
            0, CopyFromParent, InputOutput, CopyFromParent,
            CWOverrideRedirect | CWEventMask | CWBackPixmap, &swa);
        apply_overlay_hints(dpy, w);
        XMapRaised(dpy, w);
        windows.push_back(w);
    }
    XSync(dpy, False);

    // First-paint pass with a bounded wait — XWindowEvent blocks
    // forever which would deadlock the next click's join. Poll
    // for Expose with a cap; if the WM never sends one, paint
    // anyway (the override-redirect window is mapped, so the
    // paint lands).
    const auto paint_deadline = std::chrono::steady_clock::now()
                              + std::chrono::milliseconds(500);
    for (std::size_t i = 0; i < windows.size(); ++i) {
        while (std::chrono::steady_clock::now() < paint_deadline) {
            XEvent ev;
            if (XCheckTypedWindowEvent(dpy, windows[i], Expose, &ev)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (stop_flag->load()) goto teardown;
        }
        paint_overlay(dpy, windows[i], screen,
                      overlays[i].width, overlays[i].height,
                      overlays[i]);
    }
    XFlush(dpy);

    {
        // Dwell. Poll the stop_flag every 30 ms so a fresh click
        // tears the run down promptly.
        const auto end = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds(duration_ms);
        while (std::chrono::steady_clock::now() < end) {
            if (stop_flag->load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

teardown:
    for (::Window w : windows) XDestroyWindow(dpy, w);
    XCloseDisplay(dpy);
}

}  // namespace

void show_identify_overlays(const std::vector<IdentifyOverlay>& overlays,
                            int duration_ms) {
    if (overlays.empty()) return;

    StopFlag flag = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lk(g_run_mtx);
        // Signal the previous run to exit. Its worker holds its
        // own shared_ptr to the flag so this is safe even after
        // we drop our reference.
        if (g_active_stop_flag) g_active_stop_flag->store(true);
        g_active_stop_flag = flag;
    }

    // Detach: the worker self-cleans on dwell-expire or stop_flag.
    // No join — the spawning UI thread never blocks on identify.
    std::thread(run_overlays, overlays, duration_ms, flag).detach();
}

}  // namespace unio_ui::platform
