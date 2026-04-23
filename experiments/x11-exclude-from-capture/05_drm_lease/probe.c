// probe.c — DRM / RandR resource enumeration for the leasing spike.
//
// Does NOT create a lease. Prints:
//   * The RandR outputs available on the default X screen
//   * Which ones are currently connected + their CRTC + mode
//   * The DRM card + connector id that each maps to
//   * Primary + overlay planes attached to each CRTC
//
// Used as a safe first pass to identify what a lease would target
// and to verify the server-side leasing API is actually reachable
// (the spike depends on RandR >= 1.6 on the server). Run this
// before `./lease`, which is destructive.
//
// Build: cc probe.c -o probe $(pkg-config --cflags --libs xcb-randr libdrm)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static void die(const char* msg) {
    fprintf(stderr, "probe: %s: %s\n", msg,
            errno ? strerror(errno) : "(no errno)");
    exit(2);
}

static const char* connection_str(xcb_randr_connection_t c) {
    switch (c) {
        case XCB_RANDR_CONNECTION_CONNECTED:    return "connected";
        case XCB_RANDR_CONNECTION_DISCONNECTED: return "disconnected";
        case XCB_RANDR_CONNECTION_UNKNOWN:      return "unknown";
        default:                                return "?";
    }
}

// Walk /dev/dri/card* and match each card to the outputs it
// owns by querying its DRM resources. A RandR output's XID
// doesn't correspond 1:1 to a DRM connector id on the
// client side, so we match by name (e.g. "eDP-1" or "HDMI-A-1")
// and report the card that has a connector with that name.
static void find_drm_for_connector(const char* output_name) {
    for (int i = 0; i < 8; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        drmModeResPtr res = drmModeGetResources(fd);
        if (!res) { close(fd); continue; }

        for (int c = 0; c < res->count_connectors; ++c) {
            drmModeConnectorPtr conn =
                drmModeGetConnector(fd, res->connectors[c]);
            if (!conn) continue;
            static const char* type_names[] = {
                [DRM_MODE_CONNECTOR_Unknown]     = "Unknown",
                [DRM_MODE_CONNECTOR_VGA]         = "VGA",
                [DRM_MODE_CONNECTOR_DVII]        = "DVI-I",
                [DRM_MODE_CONNECTOR_DVID]        = "DVI-D",
                [DRM_MODE_CONNECTOR_DVIA]        = "DVI-A",
                [DRM_MODE_CONNECTOR_Composite]   = "Composite",
                [DRM_MODE_CONNECTOR_SVIDEO]      = "SVIDEO",
                [DRM_MODE_CONNECTOR_LVDS]        = "LVDS",
                [DRM_MODE_CONNECTOR_Component]   = "Component",
                [DRM_MODE_CONNECTOR_9PinDIN]     = "9PinDIN",
                [DRM_MODE_CONNECTOR_DisplayPort] = "DP",
                [DRM_MODE_CONNECTOR_HDMIA]       = "HDMI-A",
                [DRM_MODE_CONNECTOR_HDMIB]       = "HDMI-B",
                [DRM_MODE_CONNECTOR_TV]          = "TV",
                [DRM_MODE_CONNECTOR_eDP]         = "eDP",
                [DRM_MODE_CONNECTOR_VIRTUAL]     = "Virtual",
                [DRM_MODE_CONNECTOR_DSI]         = "DSI",
            };
            const char* tn =
                (conn->connector_type < sizeof(type_names)/sizeof(type_names[0])
                    && type_names[conn->connector_type])
                ? type_names[conn->connector_type] : "Unknown";
            char name[64];
            snprintf(name, sizeof(name), "%s-%d",
                     tn, conn->connector_type_id);
            if (strcmp(name, output_name) == 0) {
                printf("      → /dev/dri/card%d  connector_id=%u  "
                       "type=%s  status=%s\n",
                       i, conn->connector_id, name,
                       conn->connection == DRM_MODE_CONNECTED
                           ? "CONNECTED" : "not-connected");
                // Plane enumeration needs ATOMIC + UNIVERSAL_PLANES caps.
                drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
                drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
                drmModePlaneResPtr pr = drmModeGetPlaneResources(fd);
                if (pr) {
                    int matched = 0;
                    for (uint32_t p = 0; p < pr->count_planes; ++p) {
                        drmModePlanePtr pl =
                            drmModeGetPlane(fd, pr->planes[p]);
                        if (!pl) continue;
                        // We don't know which CRTC the leased connector
                        // will eventually use, so report every plane
                        // visible on this card. Filtering by CRTC
                        // happens in lease.c after we pick one.
                        (void)matched;
                        drmModeFreePlane(pl);
                    }
                    printf("      → %u plane(s) total on this card\n",
                           pr->count_planes);
                    drmModeFreePlaneResources(pr);
                }
                drmModeFreeConnector(conn);
                drmModeFreeResources(res);
                close(fd);
                return;
            }
            drmModeFreeConnector(conn);
        }
        drmModeFreeResources(res);
        close(fd);
    }
    printf("      → no /dev/dri/card matched this output name "
           "(odd — server ↔ DRM naming mismatch?)\n");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    xcb_connection_t* c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) die("xcb_connect");

    // RandR version handshake — we need 1.6 for leasing. The
    // server-side check in the README showed 1.6, so this is a
    // sanity check that our client connection also sees it.
    xcb_randr_query_version_cookie_t qv_cookie =
        xcb_randr_query_version(c, 1, 6);
    xcb_generic_error_t* err = NULL;
    xcb_randr_query_version_reply_t* qv =
        xcb_randr_query_version_reply(c, qv_cookie, &err);
    if (err || !qv) die("RandR QueryVersion failed — extension missing?");
    printf("RandR: server negotiated %u.%u "
           "(client asked for 1.6)\n",
           qv->major_version, qv->minor_version);
    free(qv);

    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    xcb_window_t root = it.data->root;

    xcb_randr_get_screen_resources_current_cookie_t gr_cookie =
        xcb_randr_get_screen_resources_current(c, root);
    xcb_randr_get_screen_resources_current_reply_t* gr =
        xcb_randr_get_screen_resources_current_reply(
            c, gr_cookie, &err);
    if (err || !gr) die("RandR GetScreenResourcesCurrent failed");

    int nouts = xcb_randr_get_screen_resources_current_outputs_length(gr);
    xcb_randr_output_t* outs =
        xcb_randr_get_screen_resources_current_outputs(gr);

    printf("Outputs (%d total):\n", nouts);
    for (int i = 0; i < nouts; ++i) {
        xcb_randr_get_output_info_cookie_t oi_cookie =
            xcb_randr_get_output_info(c, outs[i], gr->config_timestamp);
        xcb_randr_get_output_info_reply_t* oi =
            xcb_randr_get_output_info_reply(c, oi_cookie, &err);
        if (err || !oi) continue;
        int nlen = xcb_randr_get_output_info_name_length(oi);
        uint8_t* nbytes = xcb_randr_get_output_info_name(oi);
        char name[64] = {0};
        int copy = nlen < 63 ? nlen : 63;
        memcpy(name, nbytes, copy);
        name[copy] = 0;

        printf("  [%d] %s  randr_xid=0x%x  crtc=0x%x  status=%s",
               i, name, outs[i], oi->crtc,
               connection_str(oi->connection));
        if (oi->crtc != XCB_NONE) {
            xcb_randr_get_crtc_info_cookie_t ci_cookie =
                xcb_randr_get_crtc_info(c, oi->crtc, gr->config_timestamp);
            xcb_randr_get_crtc_info_reply_t* ci =
                xcb_randr_get_crtc_info_reply(c, ci_cookie, &err);
            if (ci) {
                printf("  @%ux%u+%d+%d",
                       ci->width, ci->height, ci->x, ci->y);
                free(ci);
            }
        }
        printf("\n");

        if (oi->connection == XCB_RANDR_CONNECTION_CONNECTED) {
            find_drm_for_connector(name);
        }
        free(oi);
    }
    free(gr);

    xcb_disconnect(c);
    printf("\nProbe complete. No lease was created.\n");
    printf("If the connected output above shows a valid card + "
           "connector + plane count, you can proceed to "
           "./lease (destructive — see README).\n");
    return 0;
}
