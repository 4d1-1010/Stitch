#!/usr/bin/env bash
# Run one of the mini-compositor experiments inside a nested Xephyr
# server with a specified compositor. Used for Q4 compositor
# coverage without having to boot into a different desktop session.
#
# Usage:
#   ./run_in_xephyr.sh <bare|picom|kwin>  [experiment.py]
#
# Defaults to 02_mini_compositor.py. Writes artefacts into
# ./artifacts/<compositor>_<experiment>_*.png so repeated runs
# don't overwrite each other.
#
# What it does:
#   1. Picks the next free DISPLAY in the :80..:89 range.
#   2. Launches Xephyr there, windowed at 1280x800 so visible
#      in the host session.
#   3. Optionally launches the requested compositor inside the
#      nested display (picom / kwin_x11 / nothing for bare).
#   4. Runs the given experiment with DISPLAY set to the nested
#      server. The experiment's own stdout goes through; the
#      script's job is purely orchestration + cleanup.
#   5. On exit (success, failure, or Ctrl-C), kills the compositor
#      and Xephyr.
#
# All three compositors should be tested end-to-end; the four
# validation questions in the README apply uniformly.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
COMPOSITOR="${1:-}"
EXPERIMENT="${2:-02_mini_compositor.py}"

case "$COMPOSITOR" in
    bare|picom|kwin) ;;
    *)
        echo "usage: $0 <bare|picom|kwin> [experiment.py]" >&2
        exit 2
        ;;
esac

case "$COMPOSITOR" in
    picom) command -v picom >/dev/null \
        || { echo "picom not installed — apt install picom" >&2; exit 3; } ;;
    kwin)  command -v kwin_x11 >/dev/null \
        || { echo "kwin_x11 not installed — apt install kwin-x11" >&2; exit 3; } ;;
esac

# Pick a free display number. Xephyr refuses to start if the socket
# /tmp/.X${N}-lock already exists; scan for a free one.
DISPLAY_N=""
for n in 80 81 82 83 84 85 86 87 88 89; do
    if ! [[ -e "/tmp/.X${n}-lock" ]] && ! [[ -e "/tmp/.X11-unix/X${n}" ]]; then
        DISPLAY_N=":$n"
        break
    fi
done
if [[ -z "$DISPLAY_N" ]]; then
    echo "no free DISPLAY in :80..:89" >&2
    exit 4
fi

echo "=== Xephyr on $DISPLAY_N, compositor=$COMPOSITOR ==="

Xephyr -screen 1280x800 -resizeable -noreset "$DISPLAY_N" >/dev/null 2>&1 &
XEPHYR_PID=$!
# Small settle so the server is listening before clients connect.
sleep 0.8
if ! ps -p "$XEPHYR_PID" > /dev/null; then
    echo "Xephyr failed to stay up on $DISPLAY_N" >&2
    exit 5
fi

# Paint a distinctive root-window background so we can tell whether
# XGetImage(root, ...) returns real pixels inside the nested server.
# A fresh X server's root-window contents are undefined; on Xephyr
# they read back as all-zero until something paints them, which
# would otherwise cause the capture to look uniformly black even
# when the overlay was drawn on top. Grey is distinct from our
# magenta sentinel and easy to recognise in artefact PNGs.
DISPLAY="$DISPLAY_N" xsetroot -solid grey50 2>/dev/null || true

COMPOSITOR_PID=""
case "$COMPOSITOR" in
    bare)
        # Nothing to do — Xephyr alone, no compositor in the nested
        # display. This is the "X11 without a compositor" case;
        # redirected windows have no pixmaps to feed a mini-
        # compositor, so we expect the mini-compositor capture to
        # be empty / mostly empty. That's still useful: the spike
        # verifies the capture code doesn't blow up, and it tells
        # us whether we need a fallback.
        echo "    (no compositor)"
        ;;
    picom)
        DISPLAY="$DISPLAY_N" picom --backend xrender \
            --unredir-if-possible=false \
            >/dev/null 2>&1 &
        COMPOSITOR_PID=$!
        sleep 0.5
        echo "    picom PID=$COMPOSITOR_PID"
        ;;
    kwin)
        # kwin_x11 needs at least basic session hooks; --replace
        # lets it become the compositor for this display without
        # complaining about the absence of a previous one.
        DISPLAY="$DISPLAY_N" kwin_x11 --replace \
            >/dev/null 2>&1 &
        COMPOSITOR_PID=$!
        sleep 1.2   # kwin takes longer to come up than picom
        echo "    kwin_x11 PID=$COMPOSITOR_PID"
        ;;
esac

cleanup() {
    if [[ -n "$COMPOSITOR_PID" ]]; then
        kill "$COMPOSITOR_PID" 2>/dev/null || true
    fi
    kill "$XEPHYR_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "=== running $EXPERIMENT ==="
# Ship the per-compositor tag into the experiment via env — the
# experiment's detect_compositor() reads XDG_CURRENT_DESKTOP for
# the result line, so overriding here gives clean labels like
# "xephyr-picom" instead of falling back to "unknown-compositor".
DISPLAY="$DISPLAY_N" \
XDG_CURRENT_DESKTOP="xephyr-${COMPOSITOR}" \
    "$HERE/.venv/bin/python" "$HERE/$EXPERIMENT"
