// lease.c — DRM leasing + atomic commit spike.
//
// DANGER — destructive to the running X session on the leased
// monitor. Takes over the monitor's scanout for a few seconds.
// Do NOT run inside the X session you depend on — run from a TTY
// (Ctrl-Alt-F3), or on a VM / spare machine. The probe.c tool in
// this directory enumerates resources safely without leasing.
//
// Flow:
//   1. xcb_randr_create_lease → get DRM fd with master on the
//      target connector + CRTC.
//   2. drmSetClientCap ATOMIC + UNIVERSAL_PLANES on the leased fd.
//   3. Discover the primary plane attached to the leased CRTC.
//   4. Allocate a dumb buffer, mmap, fill with SENTINEL_RGB
//      (magenta 0xFF3F7F — matches the rest of the spike).
//   5. Atomic TEST_ONLY commit. If it fails, bail before the
//      destructive real commit runs.
//   6. Atomic real commit with DRM_MODE_ATOMIC_ALLOW_MODESET.
//   7. Sleep 3 s (watchdog — SIGINT / SIGTERM release immediately).
//   8. Release lease via xcb_randr_free_lease.
//
// During steps 6-7, a Python capture harness running in parallel
// (see run_spike.sh) grabs the X framebuffer every 200 ms. Post-
// run, the harness reports whether any captured frame contained
// SENTINEL_RGB. Expected: none did — leased scan-out bypasses
// the X framebuffer entirely. This is the "hardware WDA_EXCLUDE-
// FROMCAPTURE" result we're after.
//
// Build: cc lease.c -o lease $(pkg-config --cflags --libs xcb-randr xcb libdrm)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdint.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

// Match the sentinel colour used by the Python property spike
// (harness.py). Keeps the capture-side check uniform.
#define SENTINEL_R 0xFF
#define SENTINEL_G 0x3F
#define SENTINEL_B 0x7F

#define HOLD_SECONDS 3

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void die(const char* where) {
    fprintf(stderr, "lease: %s: %s\n", where, strerror(errno));
    exit(2);
}

// Pull a DRM property id by name from an object's current
// property set — atomic modesetting identifies properties
// indirectly via id numbers the driver publishes.
static uint32_t prop_id(int fd, uint32_t obj_id, uint32_t obj_type,
                        const char* name) {
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return 0;
    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
        if (!p) continue;
        if (strcmp(p->name, name) == 0) id = p->prop_id;
        drmModeFreeProperty(p);
        if (id) break;
    }
    drmModeFreeObjectProperties(props);
    return id;
}

int main(int argc, char** argv) {
    const char* target = (argc >= 2) ? argv[1] : "eDP-1";
    fprintf(stderr, "lease: target connector = %s\n", target);
    fprintf(stderr, "lease: WARNING — monitor will go blank/magenta "
                    "for ~%d seconds during the lease.\n", HOLD_SECONDS);

    // --- 1. xcb: find the output + crtc, create the lease ---
    xcb_connection_t* c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) die("xcb_connect");

    const xcb_setup_t* setup = xcb_get_setup(c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    xcb_window_t root = it.data->root;

    xcb_randr_get_screen_resources_current_cookie_t gr_cookie =
        xcb_randr_get_screen_resources_current(c, root);
    xcb_generic_error_t* err = NULL;
    xcb_randr_get_screen_resources_current_reply_t* gr =
        xcb_randr_get_screen_resources_current_reply(c, gr_cookie, &err);
    if (err || !gr) die("GetScreenResourcesCurrent");

    int nouts = xcb_randr_get_screen_resources_current_outputs_length(gr);
    xcb_randr_output_t* outs =
        xcb_randr_get_screen_resources_current_outputs(gr);

    xcb_randr_output_t output = XCB_NONE;
    xcb_randr_crtc_t crtc = XCB_NONE;
    for (int i = 0; i < nouts; ++i) {
        xcb_randr_get_output_info_reply_t* oi =
            xcb_randr_get_output_info_reply(
                c, xcb_randr_get_output_info(
                    c, outs[i], gr->config_timestamp),
                &err);
        if (err || !oi) continue;
        int nlen = xcb_randr_get_output_info_name_length(oi);
        char name[64] = {0};
        memcpy(name, xcb_randr_get_output_info_name(oi),
               nlen < 63 ? nlen : 63);
        if (strcmp(name, target) == 0
            && oi->connection == XCB_RANDR_CONNECTION_CONNECTED
            && oi->crtc != XCB_NONE) {
            output = outs[i];
            crtc   = oi->crtc;
        }
        free(oi);
    }
    free(gr);

    if (!output || !crtc) {
        fprintf(stderr, "lease: target '%s' not found / not connected "
                        "/ no CRTC\n", target);
        xcb_disconnect(c);
        return 3;
    }

    xcb_randr_lease_t lease_id = xcb_generate_id(c);
    xcb_randr_create_lease_cookie_t cl_cookie =
        xcb_randr_create_lease(c, root, lease_id, 1, 1, &crtc, &output);
    xcb_randr_create_lease_reply_t* cl =
        xcb_randr_create_lease_reply(c, cl_cookie, &err);
    if (err || !cl) die("CreateLease — X server rejected the request");
    int* fds = xcb_randr_create_lease_reply_fds(c, cl);
    if (!fds) die("CreateLease returned no fds");
    int drm_fd = fds[0];
    fprintf(stderr, "lease: acquired, drm_fd=%d lease_id=0x%x\n",
            drm_fd, lease_id);
    free(cl);
    // Ignore from now on — xcb still owns c, but we've got the drm_fd.

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    // --- 2. Set caps + discover resources under the lease ---
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
        die("CAP UNIVERSAL_PLANES");
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0)
        die("CAP ATOMIC");

    drmModeResPtr res = drmModeGetResources(drm_fd);
    if (!res) die("drmModeGetResources (on leased fd)");
    if (res->count_crtcs != 1 || res->count_connectors != 1) {
        fprintf(stderr, "lease: expected 1 crtc + 1 connector under "
                        "the lease, got %d / %d\n",
                        res->count_crtcs, res->count_connectors);
        goto out_release;
    }
    uint32_t drm_crtc = res->crtcs[0];
    uint32_t drm_conn = res->connectors[0];

    drmModeCrtcPtr crtc_info = drmModeGetCrtc(drm_fd, drm_crtc);
    if (!crtc_info) die("drmModeGetCrtc");
    uint32_t mode_w = crtc_info->width;
    uint32_t mode_h = crtc_info->height;
    drmModeModeInfo mode = crtc_info->mode;
    fprintf(stderr, "lease: crtc=%u mode=%ux%u@%uHz\n",
            drm_crtc, mode_w, mode_h, mode.vrefresh);
    drmModeFreeCrtc(crtc_info);

    drmModePlaneResPtr pr = drmModeGetPlaneResources(drm_fd);
    if (!pr) die("drmModeGetPlaneResources");
    uint32_t primary = 0;
    for (uint32_t i = 0; i < pr->count_planes; ++i) {
        drmModePlanePtr p = drmModeGetPlane(drm_fd, pr->planes[i]);
        if (!p) continue;
        // Plane is usable for our CRTC iff its possible_crtcs
        // bitmask has bit N set where N is the CRTC's index in
        // res->crtcs. Under a lease, res->crtcs has exactly one
        // entry (index 0), so we look at bit 0.
        int attached = (p->possible_crtcs & 1u) != 0;
        uint32_t t = prop_id(drm_fd, p->plane_id,
                             DRM_MODE_OBJECT_PLANE, "type");
        // type property is an enum; the current value is in the
        // properties reply. Easier to fetch via
        // drmModeObjectGetProperties and match by name+value.
        drmModeObjectPropertiesPtr op =
            drmModeObjectGetProperties(drm_fd, p->plane_id,
                                        DRM_MODE_OBJECT_PLANE);
        uint64_t val = 0;
        for (uint32_t j = 0; j < op->count_props; ++j) {
            if (op->props[j] == t) { val = op->prop_values[j]; break; }
        }
        drmModeFreeObjectProperties(op);
        if (attached && val == DRM_PLANE_TYPE_PRIMARY) {
            primary = p->plane_id;
        }
        drmModeFreePlane(p);
        if (primary) break;
    }
    drmModeFreePlaneResources(pr);
    if (!primary) {
        fprintf(stderr, "lease: no primary plane reachable from this "
                        "lease — can't scan out.\n");
        goto out_release;
    }
    fprintf(stderr, "lease: primary plane=%u\n", primary);

    // --- 3. Allocate a dumb buffer ---
    struct drm_mode_create_dumb creq = {0};
    creq.width  = mode_w;
    creq.height = mode_h;
    creq.bpp    = 32;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        die("CREATE_DUMB");

    uint32_t handles[4] = { creq.handle, 0, 0, 0 };
    uint32_t pitches[4] = { creq.pitch,  0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t fb_id = 0;
    if (drmModeAddFB2(drm_fd, mode_w, mode_h, DRM_FORMAT_XRGB8888,
                       handles, pitches, offsets, &fb_id, 0) < 0)
        die("AddFB2");

    struct drm_mode_map_dumb mreq = {0};
    mreq.handle = creq.handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        die("MAP_DUMB");
    void* map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, drm_fd, mreq.offset);
    if (map == MAP_FAILED) die("mmap");

    // Fill with sentinel. XRGB8888 memory layout: B, G, R, X per pixel
    // on a little-endian box.
    for (uint32_t y = 0; y < mode_h; ++y) {
        uint8_t* row = (uint8_t*)map + y * creq.pitch;
        for (uint32_t x = 0; x < mode_w; ++x) {
            row[x*4 + 0] = SENTINEL_B;
            row[x*4 + 1] = SENTINEL_G;
            row[x*4 + 2] = SENTINEL_R;
            row[x*4 + 3] = 0;
        }
    }
    munmap(map, creq.size);

    // --- 4. Atomic TEST_ONLY, then real commit ---
    uint32_t mode_blob = 0;
    if (drmModeCreatePropertyBlob(drm_fd, &mode, sizeof(mode),
                                    &mode_blob) < 0)
        die("CreatePropertyBlob(mode)");

    drmModeAtomicReqPtr atomic = drmModeAtomicAlloc();
    if (!atomic) die("AtomicAlloc");

#define ADD(obj, obj_type, name, val) do { \
        uint32_t pid = prop_id(drm_fd, (obj), (obj_type), (name)); \
        if (!pid) die("prop_id " name); \
        if (drmModeAtomicAddProperty(atomic, (obj), pid, (val)) < 0) \
            die("atomic add " name); \
    } while (0)

    ADD(drm_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", drm_crtc);
    ADD(drm_crtc, DRM_MODE_OBJECT_CRTC,      "MODE_ID", mode_blob);
    ADD(drm_crtc, DRM_MODE_OBJECT_CRTC,      "ACTIVE",  1);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "FB_ID",    fb_id);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "CRTC_ID",  drm_crtc);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "SRC_X",    0);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "SRC_Y",    0);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "SRC_W",    mode_w << 16);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "SRC_H",    mode_h << 16);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "CRTC_X",   0);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "CRTC_Y",   0);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "CRTC_W",   mode_w);
    ADD(primary,  DRM_MODE_OBJECT_PLANE,     "CRTC_H",   mode_h);

    int rc = drmModeAtomicCommit(
        drm_fd, atomic,
        DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET,
        NULL);
    if (rc < 0) {
        fprintf(stderr, "lease: TEST_ONLY atomic commit rejected "
                        "by driver (rc=%d errno=%d) — aborting before "
                        "blanking the display.\n", rc, errno);
        drmModeAtomicFree(atomic);
        goto out_destroy;
    }
    fprintf(stderr, "lease: TEST_ONLY commit ok, performing real "
                    "commit now.\n");

    rc = drmModeAtomicCommit(
        drm_fd, atomic,
        DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    if (rc < 0) {
        fprintf(stderr, "lease: real atomic commit failed rc=%d "
                        "errno=%d\n", rc, errno);
        drmModeAtomicFree(atomic);
        goto out_destroy;
    }
    drmModeAtomicFree(atomic);

    // --- 5. Hold for the configured duration, or until a signal ---
    fprintf(stderr, "lease: holding for %d seconds — monitor should "
                    "be showing solid magenta.\n", HOLD_SECONDS);
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!g_stop) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - t0.tv_sec)
                       + (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed >= HOLD_SECONDS) break;
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

out_destroy:
    if (mode_blob)
        drmModeDestroyPropertyBlob(drm_fd, mode_blob);
    if (fb_id)
        drmModeRmFB(drm_fd, fb_id);
    if (creq.handle) {
        struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }

out_release:
    drmModeFreeResources(res);
    // Freeing the lease returns the connector + CRTC + planes to
    // the X server, which will resume driving the display.
    xcb_randr_free_lease(c, lease_id, 0);
    xcb_flush(c);
    fprintf(stderr, "lease: released.\n");
    close(drm_fd);
    xcb_disconnect(c);
    return 0;
}
