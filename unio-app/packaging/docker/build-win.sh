#!/bin/bash
# Build unio-app's Windows binary (unio-ui.exe) via msvc-wine in
# a Docker container and emit it to unio-app/dist/win-x64/ on the
# host.
#
# Runs entirely on the Linux orchestrator — no Windows host
# required. msvc-wine runs Microsoft's real MSVC toolchain under
# wine, so the output PE32+ binary is bit-identical to a VS 2022
# build on a native Windows machine.
#
# Usage:
#   unio-app/packaging/docker/build-win.sh              # incremental
#   unio-app/packaging/docker/build-win.sh --clean      # wipe cache
#   unio-app/packaging/docker/build-win.sh --no-cache   # force image rebuild
#
# Outputs:
#   unio-app/dist/win-x64/unio-ui.exe         (PE32+, runtime app)
#   unio-app/dist/win-x64/LICENSES.txt        (third-party bundle)
#   unio-app/dist/win-x64/build-info.txt      (commit + timestamp)
#
# Scope is unio-app only — unio-pipe is not built here, and the
# unio-license-tool is not built on Windows yet (msvc-wine has no
# OpenSSL headers to target).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
UNIO_APP_DIR="$(cd "$HERE/../.." && pwd)"

IMAGE_TAG="unio-app-win-builder:latest"
BUILD_VOLUME="unio-app-win-build-cache"
OUT_DIR="$UNIO_APP_DIR/dist/win-x64"

TARGETS=(unio-ui)

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
echo "         (first build ≈ 15 min due to vsdownload; cached after)"
docker build "${docker_build_flags[@]}" \
    -t "$IMAGE_TAG" \
    -f "$HERE/Dockerfile.win-cross" \
    "$HERE"

if (( do_clean )); then
    echo "=== wiping build cache volume $BUILD_VOLUME ==="
    docker volume rm -f "$BUILD_VOLUME" >/dev/null 2>&1 || true
fi
docker volume create "$BUILD_VOLUME" >/dev/null

mkdir -p "$OUT_DIR"

git_sha="$(git -C "$UNIO_APP_DIR" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$UNIO_APP_DIR" status --porcelain 2>/dev/null)" ]]; then
    git_sha="${git_sha}-dirty"
fi

CMAKE_CONFIGURE="CC=cl CXX=cl cmake -S /src -B /build \
    -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=AMD64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TRY_COMPILE_CONFIGURATION=Release \
    -DCMAKE_POLICY_DEFAULT_CMP0141=NEW \
    -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded \
    -DUNIO_OPENSSL_ROOT=/opt/openssl-win \
    -DUNIO_APP_BUILD_COMMIT=$git_sha"

echo "=== 2/4  cmake configure (commit=$git_sha) ==="
docker run --rm \
    -v "$UNIO_APP_DIR:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "source /etc/msvc-env.sh && $CMAKE_CONFIGURE"

echo "=== 3/4  cmake build (targets: ${TARGETS[*]}) ==="
build_targets_csv=""
for t in "${TARGETS[@]}"; do
    build_targets_csv+="--target $t "
done
docker run --rm \
    -v "$UNIO_APP_DIR:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "source /etc/msvc-env.sh && cmake --build /build -j \$(nproc) $build_targets_csv"

echo "=== 4/4  extract ==="
extract_cmd='set -e; mkdir -p /out'
for t in "${TARGETS[@]}"; do
    extract_cmd+="; cp /build/bin/$t.exe /out/$t.exe"
done
extract_cmd+="; cp /src/unio/LICENSES.txt /out/LICENSES.txt"
extract_cmd+="; echo 'commit:  $git_sha' > /out/build-info.txt"
extract_cmd+="; echo \"built:   \$(date -u +%Y-%m-%dT%H:%M:%SZ)\" >> /out/build-info.txt"
extract_cmd+="; echo \"image:   $IMAGE_TAG\" >> /out/build-info.txt"
extract_cmd+="; echo 'targets: ${TARGETS[*]}' >> /out/build-info.txt"

docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$UNIO_APP_DIR:/src:ro" \
    -v "$BUILD_VOLUME:/build:ro" \
    -v "$OUT_DIR:/out" \
    "$IMAGE_TAG" \
    "$extract_cmd"

echo
echo "=== DONE ==="
for t in "${TARGETS[@]}"; do
    echo "  $OUT_DIR/$t.exe"
    file "$OUT_DIR/$t.exe" || true
    ls -lah "$OUT_DIR/$t.exe"
done
echo
cat "$OUT_DIR/build-info.txt"
