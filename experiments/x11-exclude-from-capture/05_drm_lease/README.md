# DRM/KMS direct scanout via RandR leasing — spike

**Goal:** prove that UnIO can acquire a lease for a physical connector + CRTC + plane from the running X server, draw directly to the display via `drmModeAtomicCommit`, and confirm the X11 framebuffer that `capture_xcomposite.cpp` reads does NOT contain what we drew. If both halves are true, this is the deterministic Linux primitive for "overlay visible on screen but excluded from capture" — the moral equivalent of Windows's `WDA_EXCLUDEFROMCAPTURE`.

## Background

UnIO's swap-fullscreen scenario: P1 shows P2's desktop fullscreen on its monitor, P2 shows P1's desktop fullscreen on its monitor. Both capture their own framebuffers and stream to the other. Without exclusion this creates a mirror-to-mirror feedback loop (box in a box in a box).

Prior spike experiments (`01_property_exclude.py`, `02_mini_compositor.py`, `03_stacking_order.py`) established:

- The custom-property signal works.
- Zero-out (strategy 01) works deterministically but leaves a black rectangle.
- Mini-compositor reconstruction (strategy 02) is architecturally broken on real DEs — misses compositor-painted chrome, mishandles CSD shadows.
- DRM overlay planes via `vaPutSurface` are non-deterministic (the compositor can reject the request, silent fallback).

**This experiment is the DETERMINISTIC version of the overlay-plane idea.** Instead of asking the compositor politely for a plane and hoping, we use **RandR 1.6 leasing** (XServer-level) to take explicit ownership of a connector + CRTC + plane. Once leased, the compositor cannot revoke or steal it for the lease duration. We then use the libdrm atomic API to drive that display directly.

## The mechanism

1. **Request lease** via `xcb_randr_create_lease(connectors, crtcs)`. The X server grants ownership and returns a DRM file descriptor with master-equivalent permissions on the listed resources.
2. **Configure with libdrm**: `drmSetClientCap(DRM_CLIENT_CAP_ATOMIC, 1)` + discover planes belonging to the leased CRTC.
3. **Allocate a dumb buffer** (`DRM_IOCTL_MODE_CREATE_DUMB`), mmap, draw a test pattern (our magenta sentinel colour).
4. **Atomic commit**: `drmModeAtomicAddProperty` to set the plane's FB_ID + CRTC_ID + position + size, then `drmModeAtomicCommit(DRM_MODE_ATOMIC_ALLOW_MODESET)`. The display now shows our pattern directly from the framebuffer we own.
5. **Concurrently, a Python capture harness** grabs the X framebuffer via `XShmGetImage(root, ...)` — the same call `capture_xcomposite.cpp` makes in production. Because X no longer owns the leased connector's scan-out, the X framebuffer for that region is whatever X was painting *before* the lease was taken. The magenta sentinel is NOT in the X framebuffer.
6. **Release the lease** via `xcb_randr_free_lease`. The X server reclaims the connector + CRTC + plane and resumes rendering the user's desktop there.

## Expected result

- **On screen during the lease** (3 seconds): solid magenta.
- **X framebuffer captured during the lease**: the user's desktop, unchanged from before the lease. NO magenta sentinel pixels.
- **After the lease**: the user's desktop resumes.

PASS means both halves:
- C program reports `lease acquired` + `atomic commit succeeded`
- Python capture shows `sentinel_count=0` in every frame taken during the lease window

## Safety

Leasing takes the monitor away from X for the lease duration. With a single-monitor setup, the user's visible session is blanked / replaced by the test pattern during that window — including the terminal that launched the test. Mitigations baked into the spike:

- Short, fixed lease duration (3 seconds, watchdog-enforced).
- Signal handlers (`SIGINT`, `SIGTERM`) release immediately.
- `drmModeAtomicCommit(TEST_ONLY)` before the real commit, to catch driver rejection without blanking the display.
- All output goes to `./artifacts/drm_lease.log`, not stdout, so the terminal being covered during the lease doesn't lose information.
- Probe mode (`./probe --no-lease`) enumerates resources WITHOUT creating a lease, so the first run can verify the environment without visible disruption.

## Layout

- `probe.c` — enumerate DRM / RandR resources, identify target connector + CRTC + plane, report what a lease would cover. No lease created.
- `lease.c` — acquire lease, atomic commit a magenta test pattern, hold 3 s, release. Destructive — monitor goes dark / magenta during the lease.
- `run_spike.sh` — orchestrator. Starts the Python capture harness, waits for the user to confirm, runs `lease`, waits, reports results.
- `build.sh` — `cc` compile wrapper.
- `capture_during_lease.py` — Python harness that captures the X framebuffer every 200 ms during the lease window and reports sentinel pixel counts.

## Build deps

```
sudo apt install -y libxcb-randr0-dev libdrm-dev libx11-dev
```

(All MIT / free for commercial use. Kernel DRM is GPL-2.0 ABI, which is "use the interface, not link the code" — our userspace client has no GPL obligation.)

## How to run

First run the probe (safe, no lease created):

```
cd experiments/x11-exclude-from-capture/05_drm_lease/
./build.sh
./probe
```

Probe output identifies which card / connector / CRTC / plane the spike would target. If everything looks right, run the destructive lease test:

```
./run_spike.sh
```

That launches the Python capture in the background, then runs `lease` (monitor goes magenta for ~3s), then reports whether the sentinel appeared in any captured frame.

## Fallback path

If leasing fails on this system (the X server may deny the grant if it can't release the only connected monitor without breaking the user's session), we fall back to strategy 01 (zero-out) as the documented X11 behaviour. Wayland (future) would use `wp_drm_lease_device_v1` + the same atomic commit path, which is the Wayland-native equivalent.
