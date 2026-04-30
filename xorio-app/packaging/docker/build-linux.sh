#!/bin/bash
# Build xorio-app's Linux binaries (xorio + xorio-license-tool)
# in a Docker container and emit them to xorio-app/dist/linux-x64/
# on the host.
#
# Usage:
#   xorio-app/packaging/docker/build-linux.sh              # incremental
#   xorio-app/packaging/docker/build-linux.sh --clean      # wipe build cache
#   xorio-app/packaging/docker/build-linux.sh --no-cache   # force image rebuild
#
# Outputs:
#   xorio-app/dist/linux-x64/xorio              (ELF, runtime app)
#   xorio-app/dist/linux-x64/xorio-license-tool    (ELF, dev CLI)
#   xorio-app/dist/linux-x64/LICENSES.txt         (third-party bundle)
#   xorio-app/dist/linux-x64/build-info.txt       (commit + timestamp)
#
# Scope is xorio-app only — unio-pipe is not built here. For a
# whole-repo build use packaging/docker/build-linux.sh at the
# repo root.
#
# Implementation notes:
#   * Source tree bind-mounted READ-ONLY. /build is a named
#     docker volume so cmake + FetchContent caches survive
#     between runs.
#   * Configure + build run as root inside the container; extract
#     runs as the host user so files in dist/ stay user-owned.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
XORIO_APP_DIR="$(cd "$HERE/../.." && pwd)"

IMAGE_TAG="xorio-app-linux-builder:latest"
BUILD_VOLUME="xorio-app-linux-build-cache"
OUT_DIR="$XORIO_APP_DIR/dist/linux-x64"

TARGETS=(xorio xorio-license-tool)

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

git_sha="$(git -C "$XORIO_APP_DIR" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$XORIO_APP_DIR" status --porcelain 2>/dev/null)" ]]; then
    git_sha="${git_sha}-dirty"
fi

echo "=== 2/4  cmake configure (commit=$git_sha) ==="
docker run --rm \
    -v "$XORIO_APP_DIR:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "cmake -S /src -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DXORIO_APP_BUILD_COMMIT=$git_sha"

echo "=== 3/4  cmake build (targets: ${TARGETS[*]}) ==="
build_targets_csv=""
for t in "${TARGETS[@]}"; do
    build_targets_csv+="--target $t "
done
docker run --rm \
    -v "$XORIO_APP_DIR:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "cmake --build /build -j \$(nproc) $build_targets_csv"

echo "=== 4/4  extract + strip ==="
extract_cmd='set -e; mkdir -p /out'
for t in "${TARGETS[@]}"; do
    extract_cmd+=" && cp /build/bin/$t /out/$t && strip /out/$t"
done
extract_cmd+=" && cp /src/xorio/LICENSES.txt /out/LICENSES.txt"
extract_cmd+=" && echo 'commit:  $git_sha' > /out/build-info.txt"
extract_cmd+=" && echo \"built:   \$(date -u +%Y-%m-%dT%H:%M:%SZ)\" >> /out/build-info.txt"
extract_cmd+=" && echo \"image:   $IMAGE_TAG\" >> /out/build-info.txt"
extract_cmd+=" && echo 'targets: ${TARGETS[*]}' >> /out/build-info.txt"

docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$XORIO_APP_DIR:/src:ro" \
    -v "$BUILD_VOLUME:/build:ro" \
    -v "$OUT_DIR:/out" \
    "$IMAGE_TAG" \
    "$extract_cmd"

echo
echo "=== DONE ==="
for t in "${TARGETS[@]}"; do
    echo "  $OUT_DIR/$t"
    file "$OUT_DIR/$t"
    ls -lah "$OUT_DIR/$t"
done
echo
cat "$OUT_DIR/build-info.txt"
