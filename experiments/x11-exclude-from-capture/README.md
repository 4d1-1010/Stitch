# X11 capture-exclude spike — custom-property protocol

**Spike / research only** — not part of the `unio-pipe` build. Branch `spike/x11-capture-exclude`. When the approach proves out, it folds into `unio-pipe/src/capture_xcomposite.cpp`; this directory stays as the audit trail.

## The problem

UnIO's display-swap feature draws an overlay window on a physical monitor to show another machine's desktop content. In the L4↔W1 swap pattern both sides stream at once:

- Windows monitor shows Linux desktop content (via overlay on Windows side).
- Linux monitor shows Windows desktop content (via overlay on Linux side).

Each side simultaneously captures its own desktop to stream to the other. On Windows the overlay is flagged `WDA_EXCLUDEFROMCAPTURE` and WGC screenshots see through it — no feedback. On Linux X11 there is no documented per-window exclude-from-capture primitive, so the overlay IS visible to our XComposite-based capture, which feeds it back to the remote, which paints it, which is captured again — fractal collapse within a few frames.

## Rejected: `XCompositeUnredirectWindow`

Short version: it removes the window from the compositing system entirely rather than excluding it from our capture. After `XCompositeUnredirectWindow(overlay)` the X server stops keeping an offscreen pixmap for the overlay and paints it directly to the framebuffer. That means:

- We can no longer capture its pixels even when we want to (e.g. to ROUTE that window somewhere, or to show it in a preview).
- Behaviour depends on the compositor; xfwm is spotty.
- Alpha blending with what's behind breaks.
- Requires the calling client to have previously called `RedirectWindow` on that window, which under Mutter / KWin we haven't (the compositor holds the redirect via `RedirectSubwindows(root)`). `BadValue` every time we tried.

The mental distinction: XComposite gives you building blocks (per-window offscreen pixmaps); `UnredirectWindow` *removes* a block. We want *selective use* of the blocks, not removal.

## The approach — custom X11 property + filter during composition

We define our own X11 atom on the overlay window:

```c
Atom prop = XInternAtom(display, "UNIO_CAPTURE_EXCLUDE", False);
int32_t value = 1;
XChangeProperty(display, overlay_window, prop,
                XA_INTEGER, 32, PropModeReplace,
                (unsigned char*)&value, 1);
```

The capture loop in `capture_xcomposite.cpp` walks the root window's children, reads the property on each, and **skips** windows where `UNIO_CAPTURE_EXCLUDE == 1` when composing its output frame. Windows without the property (the vast majority — everyone else's apps) are composed normally.

Why this wins over alternatives:

| Method | Stable | Flexible | Verdict |
|---|---|---|---|
| `WM_NAME` matching | ❌ changes | medium | ❌ |
| `WM_CLASS` matching | 🟡 stable | limited | 🟡 |
| PID matching | 🟡 stable | weak | 🟡 |
| `XCompositeUnredirectWindow` | ❌ removes window from the compositing system entirely — can't capture or route it either | — | ❌ (see "Rejected" above) |
| **Custom property** | 🟢 fully under our control | 🟢 supports future tags (e.g. `UNIO_ROUTING_TAG="pc-3-display-2"`) | ✅ **chosen** |

This scales beyond "exclude from capture". The same protocol supports future per-window metadata — e.g. a routing tag that tells the capture which remote the window's content should be streamed to, or a priority hint for frame scheduling.

## Important truth the spike uncovered

`unio-pipe/src/capture_xcomposite.cpp` doesn't actually use XComposite for the capture despite the filename — it does `XShmGetImage(dpy, root, ...)`, which is a framebuffer read. Under that model:

- "Visible to user" = "in the framebuffer". There is no mechanism that keeps a window visible on screen but absent from a framebuffer grab.
- `XCompositeUnredirectWindow`, `_NET_WM_BYPASS_COMPOSITOR`, and every other compositor-level trick is *structurally* irrelevant — they affect the compositor's offscreen chain, not the framebuffer we read.

Consequence: there are only three real options, and we ranked them:

| Option | Approach | Verdict |
|---|---|---|
| 🟡 **1** | Framebuffer grab + zero-out tagged rectangles | Fallback / debug path. Works today but leaves black holes; still *captures* excluded content (security smell); not true exclusion. |
| 🟢 **2** | Mini-compositor: walk root's children, build output from offscreen pixmaps, skip tagged windows during composition | **Target architecture.** True exclusion, no black holes, matches the pipeline vision (per-window routing, per-display composition). |
| 🔴 **3** | DRM / overlay plane: bypass X11 for the overlay rendering | Massive complexity, not portable, breaks desktop integration. Out of scope. |

## What the spike validates

Two experiments land the **Option 2 / Mini-Compositor** architecture as a CPU-path prototype (Phase 1 of the plan below). A GPU version is Phase 2 and lives outside this spike.

### `01_property_exclude.py` — Option 1 reference

Lightweight proof of the custom-property *signal*. Capture via `XGetImage(root)` then zero out the tagged rectangle client-side. Lands as the simplest possible integration and proves the property protocol carries the exclusion end-to-end. Reference only — not what we want in production.

### `02_mini_compositor.py` — Option 2 (target architecture)

The real thing. Walks root's direct children, reads each window's redirected offscreen pixmap via `XGetImage(window, ...)`, composites in stacking order, skipping any window tagged with `UNIO_CAPTURE_EXCLUDE=1`. No framebuffer grab, no post-processing.

### Current Phase 1 results (adi-pc, GNOME/Mutter X11)

```
Q1: Does exclusion fully remove the window?
  naive (framebuffer):    VISIBLE  sentinel_pixels=40000
  mini-compositor:        HIDDEN   sentinel_pixels=0
  → PASS

Q2: Do underlying windows appear correctly (no black hole)?
  samples inside the overlay rect in mini output:
    (125,125): (249, 249, 249)   — light grey (real pixels, not black)
    (220,220): (249, 249, 249)
    (315,315): (255, 255, 255)   — white (real pixels, not black)
  → PASS

Q3: Is stacking order correct?
  Iterated in the order `query_tree` returns, which is the server's
  bottom-to-top stacking order. Single-overlay spike doesn't stress
  Z-ordering across multiple overlapping tagged + untagged windows —
  outstanding as a follow-up test case.
  → PARTIAL (architecturally correct, more coverage needed)

Q4: Any compositor-specific breakage?
  Tested via `run_in_xephyr.sh <bare|picom|kwin>`, nested Xephyr
  servers so we don't need to reboot into different sessions:
    Mutter (outer GNOME session):  PASS
    Xephyr + bare (no compositor): PASS  — still works because
                                          the mini-compositor
                                          falls through to
                                          whichever children
                                          happen to be mapped
    Xephyr + picom:                PASS
    Xephyr + kwin_x11:             PASS
  → PASS on all four tested compositors
```

Phase 1 demonstrates the architecture works. Phases below are the forward path.

## Phase 2 — GPU composition (production target)

The Phase 1 CPU-path reads each window's pixmap with `XGetImage` and composites on the CPU via Pillow. That reintroduces CPU copies we worked hard to avoid in the zero-copy NVENC / AMF encoder paths on Windows. The production implementation replaces `XGetImage → CPU blit` with:

```
XCompositeNameWindowPixmap(window) → EGLImage → GPU texture → shader composite → encoder input
```

This keeps:

- **Latency**: one GPU-side composite, fed straight into the encoder's existing zero-copy-capable input path.
- **Pipeline alignment**: the encoder already accepts GPU textures on the platforms we've invested in (`encoder_nvenc_linux` via CUDA, Windows NVENC / AMF via D3D11). Linux VA-API's encoder input is VASurface, which can be EGL-backed; so we align across all vendors.
- **Correctness of the exclusion**: identical to Phase 1 — the pass skips tagged windows during composition. The only difference is the media the compositing happens in.

Phase 2 lands inside `capture_xcomposite.cpp` once Phase 1 validates across the compositors in the Q4 list above.

## Watch-out

Some compositors optimise redirected windows in ways that can show up as:

- Stale frames in the offscreen pixmap when a window hasn't repainted recently.
- Delayed pixmap updates lagging behind framebuffer updates under load.

Rare, but worth an eye when running the spike on KWin and xfwm.

## Running

```bash
cd experiments/x11-exclude-from-capture/
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

# Outer-session runs (uses the compositor you're already in):
.venv/bin/python 01_property_exclude.py     # Option 1 reference
.venv/bin/python 02_mini_compositor.py      # Option 2 (target architecture)

# Nested-Xephyr runs for Q4 coverage. Needs picom + kwin-x11
# installed: apt install picom kwin-x11.
./run_in_xephyr.sh bare                     # Xephyr + no compositor
./run_in_xephyr.sh picom                    # Xephyr + picom
./run_in_xephyr.sh kwin                     # Xephyr + kwin_x11
```

Output is a single verdict line per capture path:

```
RESULT naive:    VISIBLE (sentinel_pixels=40000, ...)
RESULT property: HIDDEN  (sentinel_pixels=0, ...)
VERDICT: PASS — custom-property exclusion works on <compositor>
```

Captured images drop into `./artifacts/` for visual confirmation.

## Target compositor coverage

Goal: `PASS` on every X11 session UnIO might be installed under.

- GNOME / Mutter (X11 session)
- KDE / KWin (X11 session)
- XFCE / xfwm + picom
- Bare X11 (`startx` with no compositor)

Property-aware filtering is compositor-agnostic by construction — it operates on the client side of the capture path, not on the compositor's drawing — so we expect all four to pass. The spike confirms this empirically.

## Out of scope

- **Wayland.** PipeWire-portal capture targets specific sources by design; feedback-avoidance is built in. Out of scope here.
- **Windows.** `WDA_EXCLUDEFROMCAPTURE` already works; see `project_wgc_wda_black.md`.
- **Reconstructing the window behind the tagged overlay.** The spike uses "zero out the rectangle"; production may choose to composite-from-scratch to fill it with what's underneath. That's a quality improvement, not a correctness question for this spike.
