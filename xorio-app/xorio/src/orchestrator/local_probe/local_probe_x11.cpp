/// @file local_probe_x11.cpp
/// @brief X11 RandR implementation of @ref ILocalProbeAdapter.
///
/// Scope: enumerate active CRTC outputs (i.e. monitors with a
/// non-zero geometry) on the default X display. Encoder / decoder
/// / presenter / capture-backend lists are intentionally left
/// empty — populating those is the unio-pipe probe's job once the
/// pipe layer folds into the same binary.
///
/// Failures (no DISPLAY, RandR missing, server refuses connection)
/// surface as an empty `displays` vector; the rest of the app
/// keeps working with no caps reported.

#include "orchestrator/local_probe/local_probe.hpp"

#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>

#include <unistd.h>

#include <memory>
#include <string>

namespace xorio_ui::orchestrator {

namespace {

/// @brief Capture the current host name into the CapsRecord's
/// machine_id / display_name fields. The orchestrator overwrites
/// machine_id again on publish — kept here for symmetry with the
/// Win32 twin.
std::string host_label() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return buf;
    return "unknown-host";
}

class X11LocalProbe final : public ILocalProbeAdapter {
public:
    CapsRecord probe() const override {
        CapsRecord r;
        r.machine_id   = host_label();
        r.display_name = r.machine_id;

        // X11 typedefs `Display` at global scope; our own
        // orchestrator::Display struct shadows it inside this
        // namespace. Fully-qualify the X11 alias so the two
        // never collide.
        using XDisplay = ::Display;
        XDisplay* dpy = ::XOpenDisplay(nullptr);
        if (dpy == nullptr) return r;

        ::Window root = DefaultRootWindow(dpy);
        XRRScreenResources* res = ::XRRGetScreenResourcesCurrent(dpy, root);
        if (res == nullptr) {
            ::XCloseDisplay(dpy);
            return r;
        }

        int number = 1;
        for (int i = 0; i < res->noutput; ++i) {
            XRROutputInfo* oi = ::XRRGetOutputInfo(dpy, res, res->outputs[i]);
            if (oi == nullptr) continue;
            // RR_Connected = 0 in xrandr.h. We don't include the
            // numeric constant explicitly to avoid a transitive
            // dep on RandR's private headers — the value is
            // documented stable across versions.
            const bool connected = (oi->connection == 0);
            const bool has_crtc  = (oi->crtc != 0);
            if (!connected || !has_crtc) {
                ::XRRFreeOutputInfo(oi);
                continue;
            }

            XRRCrtcInfo* ci = ::XRRGetCrtcInfo(dpy, res, oi->crtc);
            if (ci != nullptr) {
                if (ci->width > 0 && ci->height > 0) {
                    // `Display` here resolves via namespace lookup
                    // to the orchestrator's Display struct, not
                    // the X11 typedef (which we aliased as
                    // XDisplay above).
                    Display d;
                    d.machine_id  = r.machine_id;
                    d.monitor_id  = oi->name ? std::string(oi->name)
                                              : "DISPLAY"
                                              + std::to_string(number);
                    d.global_x    = static_cast<std::int32_t>(ci->x);
                    d.global_y    = static_cast<std::int32_t>(ci->y);
                    d.width       = static_cast<std::int32_t>(ci->width);
                    d.height      = static_cast<std::int32_t>(ci->height);
                    d.number      = number++;
                    r.displays.push_back(std::move(d));
                }
                ::XRRFreeCrtcInfo(ci);
            }
            ::XRRFreeOutputInfo(oi);
        }

        ::XRRFreeScreenResources(res);
        ::XCloseDisplay(dpy);
        return r;
    }
};

}  // namespace

std::unique_ptr<ILocalProbeAdapter> make_local_probe() {
    return std::make_unique<X11LocalProbe>();
}

}  // namespace xorio_ui::orchestrator
