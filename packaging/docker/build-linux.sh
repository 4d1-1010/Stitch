#!/bin/bash
# Build the Linux unio-pipe binary in a Docker container and emit
# it to dist/linux-x64/unio-pipe on the host.
#
# Usage:
#   packaging/docker/build-linux.sh               # incremental build
#   packaging/docker/build-linux.sh --clean       # wipe build cache first
#   packaging/docker/build-linux.sh --no-cache    # force docker image rebuild
#
# Produces:
#   dist/linux-x64/unio-pipe          (ELF 64-bit x86-64, stripped)
#   dist/linux-x64/build-info.txt     (git commit + build timestamp)
#
# Implementation notes:
#   * Source tree is bind-mounted read-only. Image has no source
#     copy, so the same image builds any branch.
#   * Build artefacts live in a named Docker volume so cmake +
#     FetchContent don't redo multi-minute work on every run.
#   * The git commit is computed on the host and handed into the
#     container via -DUNIO_BUILD_COMMIT=<sha>, matching the
#     matrix_test.py --sync convention so helper_caps.build_commit
#     reports the same value as a native host build would.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

IMAGE_TAG="unio-pipe-linux-builder:latest"
BUILD_VOLUME="unio-pipe-linux-build-cache"
OUT_DIR="$REPO_ROOT/dist/linux-x64"

do_clean=0
docker_build_flags=()
for arg in "$@"; do
    case "$arg" in
        --clean)    do_clean=1 ;;
        --no-cache) docker_build_flags+=("--no-cache") ;;
        *) echo "unknown flag: $arg" >&2; exit 2 ;;
    esac
done

echo "=== 1/4  docker build $IMAGE_TAG ==="
docker build "${docker_build_flags[@]}" \
    -t "$IMAGE_TAG" \
    -f "$HERE/Dockerfile.linux" \
    "$HERE"

if (( do_clean )); then
    echo "=== wiping build cache volume $BUILD_VOLUME ==="
    docker volume rm -f "$BUILD_VOLUME" >/dev/null 2>&1 || true
fi
docker volume create "$BUILD_VOLUME" >/dev/null

mkdir -p "$OUT_DIR"

# Git state captured on the host — the container doesn't see .git
# (we could bind-mount it but that leaks arbitrary git hooks / refs
# into a sandbox we're trying to keep hermetic).
git_sha="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    git_sha="${git_sha}-dirty"
fi

echo "=== 2/4  cmake configure (commit=$git_sha, toolchain=gcc-13) ==="
docker run --rm \
    -v "$REPO_ROOT:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "cmake -S /src/unio-pipe -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DUNIO_BUILD_COMMIT=$git_sha"

echo "=== 3/4  cmake build ==="
docker run --rm \
    -v "$REPO_ROOT:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "cmake --build /build -j \$(nproc) --target unio-pipe"

echo "=== 4/4  extract + strip ==="
# Ship:
#   unio-pipe          our binary (stripped)
#   libmsquic.so.2     QUIC transport — linked via DT_NEEDED,
#                      not packaged by any Linux distro; we build
#                      it from source via FetchContent, so we
#                      have to ship the resulting .so alongside.
#                      Preserve the SONAME symlink chain so
#                      ld.so can resolve "libmsquic.so.2".
# System libs (libva, libEGL, libX11, libcrypto, libstdc++)
# come from the target's own package manager — Ubuntu 24.04-era
# distros have them at matching ABI.
docker run --rm \
    -v "$BUILD_VOLUME:/build:ro" \
    -v "$OUT_DIR:/out" \
    "$IMAGE_TAG" \
    "cp /build/unio-pipe /out/unio-pipe \
        && strip /out/unio-pipe \
        && cp -a /build/_deps/msquic-build/bin/Release/libmsquic.so* /out/ \
        && strip /out/libmsquic.so.2 2>/dev/null || true \
        && echo 'commit: $git_sha' > /out/build-info.txt \
        && echo \"built: \$(date -u +%Y-%m-%dT%H:%M:%SZ)\" >> /out/build-info.txt \
        && echo \"image: $IMAGE_TAG\" >> /out/build-info.txt"

echo
echo "=== DONE ==="
echo "  binary:  $OUT_DIR/unio-pipe"
file "$OUT_DIR/unio-pipe"
ls -lah "$OUT_DIR/unio-pipe"
cat "$OUT_DIR/build-info.txt"
