#!/usr/bin/env bash
# Build the DRM leasing spike binaries. Plain `cc`, no CMake — the
# spike is two small TUs and the deps are stable.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# Resolve cflags / libs via pkg-config so we don't hardcode paths
# across distros. libxcb-randr is the RandR 1.6 client (system
# libXrandr 1.5.2 on Ubuntu 24.04 lacks the leasing APIs), libdrm
# for the atomic modesetting side of the lease.
CFLAGS="-Wall -Wextra -Werror -O2 -g $(pkg-config --cflags xcb-randr xcb libdrm)"
LIBS="$(pkg-config --libs xcb-randr xcb libdrm)"

echo "=== build probe ==="
cc $CFLAGS probe.c -o probe $LIBS
echo "    -> $HERE/probe"

if [[ -f lease.c ]]; then
    echo "=== build lease ==="
    cc $CFLAGS lease.c -o lease $LIBS
    echo "    -> $HERE/lease"
fi

echo "done."
