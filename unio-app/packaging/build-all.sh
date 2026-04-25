#!/bin/bash
# Build unio-app for every supported platform in one command.
#
# Runs the Linux + Windows Docker-driven builds back-to-back.
# Both produce native binaries on the same Linux orchestrator
# (msvc-wine cross-compiles the Windows side).
#
# Usage:
#   unio-app/packaging/build-all.sh              # incremental
#   unio-app/packaging/build-all.sh --clean      # wipe both build caches
#   unio-app/packaging/build-all.sh --linux      # Linux only
#   unio-app/packaging/build-all.sh --win        # Windows only
#   unio-app/packaging/build-all.sh --no-cache   # force image rebuild
#
# Outputs land at:
#   unio-app/dist/linux-x64/   (unio-ui, unio-license-tool, LICENSES, build-info)
#   unio-app/dist/win-x64/     (unio-ui.exe, LICENSES, build-info)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DOCKER_DIR="$HERE/docker"

# ── Preflight ──────────────────────────────────────────────────
if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker is not installed or not on PATH." >&2
    echo "       install Docker Engine: https://docs.docker.com/engine/install/" >&2
    exit 1
fi
if ! docker info >/dev/null 2>&1; then
    echo "error: docker daemon is not reachable." >&2
    echo "       try: sudo systemctl start docker" >&2
    echo "       or:  add your user to the 'docker' group + log out/in" >&2
    exit 1
fi

do_linux=1
do_win=1
forwarded_flags=()
for arg in "$@"; do
    case "$arg" in
        --linux)    do_win=0 ;;
        --win)      do_linux=0 ;;
        --clean|--no-cache) forwarded_flags+=("$arg") ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

# ── Linux ──────────────────────────────────────────────────────
if (( do_linux )); then
    echo
    echo "###  building unio-app for Linux  ###"
    "$DOCKER_DIR/build-linux.sh" "${forwarded_flags[@]}"
fi

# ── Windows ────────────────────────────────────────────────────
if (( do_win )); then
    echo
    echo "###  building unio-app for Windows (msvc-wine)  ###"
    "$DOCKER_DIR/build-win.sh" "${forwarded_flags[@]}"
fi

echo
echo "###  ALL BUILDS COMPLETE  ###"
