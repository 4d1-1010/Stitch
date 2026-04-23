# X11 capture-exclude spike — custom-property protocol

**Spike / research only** — not part of the `unio-pipe` build. Branch `spike/x11-capture-exclude`. When the approach proves out, it folds into `unio-pipe/src/capture_xcomposite.cpp`; this directory stays as the audit trail.

## The problem

UnIO's display-swap feature draws an overlay window on a physical monitor to show another machine's desktop content. In the L4↔W1 swap pattern both sides stream at once:

- Windows monitor shows Linux desktop content (via overlay on Windows side).
- Linux monitor shows Windows desktop content (via overlay on Linux side).

Each side simultaneously captures its own desktop to stream to the other. On Windows the overlay is flagged `WDA_EXCLUDEFROMCAPTURE` and WGC screenshots see through it — no feedback. On Linux X11 there is no documented per-window exclude-from-capture primitive, so the overlay IS visible to our XComposite-based capture, which feeds it back to the remote, which paints it, which is captured again — fractal collapse within a few frames.

## The approach — custom X11 property

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
| `XCompositeUnredirectWindow` | 🟡 works but breaks alpha + v-sync | medium | 🟡 |
| **Custom property** | 🟢 fully under our control | 🟢 supports future tags (e.g. `UNIO_ROUTING_TAG="pc-3-display-2"`) | ✅ **chosen** |

This scales beyond "exclude from capture". The same protocol supports future per-window metadata — e.g. a routing tag that tells the capture which remote the window's content should be streamed to, or a priority hint for frame scheduling.

## What the spike validates

A single experiment (`01_property_exclude.py`) demonstrates the round trip:

1. Create an overlay window painted solid magenta (`#FF3F7F`, our sentinel colour — unlikely to appear on a typical desktop).
2. Set `UNIO_CAPTURE_EXCLUDE=1` on it via `XChangeProperty`.
3. Capture the root window's composited pixels **twice**:
   - **Naive path**: `XGetImage(root, …)` — what `capture_xcomposite.cpp` does today. Expected to show magenta (the overlay IS on screen).
   - **Property-aware path**: same capture, then zero out every rectangle belonging to a `UNIO_CAPTURE_EXCLUDE=1` window. Expected to show NO magenta.
4. Verdict: strategy wins if naive = `VISIBLE` and property-aware = `HIDDEN` on the same run.

The property-aware path's "zero-out the tagged rectangle" is the simplest possible implementation of the protocol. Production can substitute a more elaborate composition (e.g. composite from per-window offscreen pixmaps via `XCompositeNameWindowPixmap`) that reconstructs what was *behind* the tagged window rather than leaving a black hole — but the spike doesn't need to prove that part; it only needs to prove the **signal** (custom property as the exclusion key) works end-to-end.

## Running

```bash
cd experiments/x11-exclude-from-capture/
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python 01_property_exclude.py
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
