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

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace xorio::orchestrator {

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

    void apply_arrangement(
        const std::vector<DisplayPlacement>& placements) const override {
        if (placements.empty()) return;

        using XDisplay = ::Display;
        XDisplay* dpy = ::XOpenDisplay(nullptr);
        if (dpy == nullptr) return;
        ::Window root = DefaultRootWindow(dpy);
        XRRScreenResources* res = ::XRRGetScreenResourcesCurrent(dpy, root);
        if (res == nullptr) {
            ::XCloseDisplay(dpy);
            return;
        }

        // Index the placement requests by monitor_id so we can
        // look each up in O(1) while walking outputs.
        std::unordered_map<std::string, DisplayPlacement> want;
        want.reserve(placements.size());
        for (const auto& p : placements) want.emplace(p.monitor_id, p);

        // RandR requires the root screen size to be at least as
        // large as the union of all CRTC rects, otherwise
        // XRRSetCrtcConfig will fail with BadMatch. Compute the
        // post-move bounding box first across all CRTCs (those
        // we move + those we leave alone), then resize the screen
        // before issuing per-CRTC moves.
        struct CrtcMove {
            RRCrtc      crtc;
            int         new_x;
            int         new_y;
            int         w;
            int         h;
            RRMode      mode;
            Rotation    rot;
            std::vector<RROutput> outs;
        };
        std::vector<CrtcMove> moves;

        int max_right  = 0;
        int max_bottom = 0;
        for (int i = 0; i < res->ncrtc; ++i) {
            XRRCrtcInfo* ci = ::XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
            if (ci == nullptr) continue;
            if (ci->mode == 0 || ci->width == 0 || ci->height == 0) {
                ::XRRFreeCrtcInfo(ci);
                continue;
            }
            // What's the monitor_id for this CRTC? RandR exposes
            // monitor names per output; an active CRTC drives one
            // or more outputs. Use the first output's name.
            std::string mid;
            for (int oi = 0; oi < ci->noutput; ++oi) {
                XRROutputInfo* o = ::XRRGetOutputInfo(dpy, res, ci->outputs[oi]);
                if (o && o->name) {
                    mid = o->name;
                    ::XRRFreeOutputInfo(o);
                    break;
                }
                if (o) ::XRRFreeOutputInfo(o);
            }

            int new_x = ci->x;
            int new_y = ci->y;
            auto it = want.find(mid);
            if (it != want.end()) {
                new_x = it->second.x;
                new_y = it->second.y;
            }

            CrtcMove m;
            m.crtc  = res->crtcs[i];
            m.new_x = new_x;
            m.new_y = new_y;
            m.w     = static_cast<int>(ci->width);
            m.h     = static_cast<int>(ci->height);
            m.mode  = ci->mode;
            m.rot   = ci->rotation;
            m.outs.assign(ci->outputs, ci->outputs + ci->noutput);
            moves.push_back(std::move(m));

            max_right  = std::max(max_right,  new_x + static_cast<int>(ci->width));
            max_bottom = std::max(max_bottom, new_y + static_cast<int>(ci->height));
            ::XRRFreeCrtcInfo(ci);
        }

        // RandR requires non-negative CRTC origins after the
        // screen resize. The caller normalises via the
        // orchestrator (smallest x/y → 0) so this is just an extra
        // safety pin: shift everything so min is at (0,0).
        int min_x = 0, min_y = 0;
        for (const auto& m : moves) {
            min_x = std::min(min_x, m.new_x);
            min_y = std::min(min_y, m.new_y);
        }
        if (min_x < 0 || min_y < 0) {
            for (auto& m : moves) {
                m.new_x -= min_x;
                m.new_y -= min_y;
            }
            max_right  -= min_x;
            max_bottom -= min_y;
        }

        if (max_right > 0 && max_bottom > 0) {
            ::XRRSetScreenSize(
                dpy, root,
                max_right, max_bottom,
                // Physical mm dims kept in proportion to logical
                // pixels using a default 96 DPI; RandR doesn't
                // really use these for layout, just for hint.
                (max_right  * 254) / (96 * 10),
                (max_bottom * 254) / (96 * 10));
        }

        // Disable then re-enable each CRTC at its new position. A
        // direct SetCrtcConfig with a new x/y also works in many
        // drivers, but disable/enable is the only path that's
        // robust across NVIDIA/AMD/Intel + Mesa quirks when the
        // resulting screen size also changed.
        for (const auto& m : moves) {
            ::XRRSetCrtcConfig(
                dpy, res, m.crtc, CurrentTime,
                0, 0, None, RR_Rotate_0, nullptr, 0);
        }
        for (const auto& m : moves) {
            ::XRRSetCrtcConfig(
                dpy, res, m.crtc, CurrentTime,
                m.new_x, m.new_y, m.mode, m.rot,
                const_cast<RROutput*>(m.outs.data()),
                static_cast<int>(m.outs.size()));
        }

        ::XRRFreeScreenResources(res);
        ::XSync(dpy, False);
        ::XCloseDisplay(dpy);
        std::fprintf(stderr,
                     "apply_arrangement: x11 placed %zu monitor(s)\n",
                     placements.size());
    }
};

}  // namespace

std::unique_ptr<ILocalProbeAdapter> make_local_probe() {
    return std::make_unique<X11LocalProbe>();
}

}  // namespace xorio::orchestrator
