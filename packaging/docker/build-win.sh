#!/bin/bash
# Build the repo's Windows binaries via msvc-wine in a Docker
# container and emit them to dist/win-x64/ on the host.
#
# Runs entirely on the Linux orchestrator — no Windows host
# required. msvc-wine runs Microsoft's real MSVC toolchain under
# wine, so the output PE32+ binaries are bit-identical to a VS
# 2022 build on a native Windows machine.
#
# Usage:
#   packaging/docker/build-win.sh              # incremental
#   packaging/docker/build-win.sh --clean      # wipe build volume
#   packaging/docker/build-win.sh --no-cache   # force image rebuild
#
# Produces (subject to the UNIO_BUILD_* cmake options — all ON by default):
#   dist/win-x64/unio-pipe.exe       (PE32+, MSVC 17.x; single-file
#                                     ship: msquic + openssl3 +
#                                     libvpl all statically linked,
#                                     MSVC CRT via /MT)
#   dist/win-x64/unio-ui.exe         (PE32+, MSVC 17.x; D3D11 + user32
#                                     + gdi32 linked dynamically from
#                                     Windows system libs, same as any
#                                     Windows desktop app)
#   dist/win-x64/build-info.txt      (git commit + build timestamp)
#
# First invocation:
#   - `docker build` pulls ~4 GB of MSVC + Windows SDK via
#     vsdownload.py. Takes ~15 min on a fast connection.
#   - After that the toolchain layer is cached; only our source
#     changes trigger rebuilds of our own TUs.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

IMAGE_TAG="unio-win-builder:latest"
BUILD_VOLUME="unio-win-build-cache"
OUT_DIR="$REPO_ROOT/dist/win-x64"

# Targets produced by this script. Edit this list (and the
# UNIO_BUILD_* cmake options in the top-level CMakeLists.txt) when
# a new binary is added to the repo.
TARGETS=(unio-pipe unio-ui)

do_clean=0
docker_build_flags=()
for arg in "$@"; do
    case "$arg" in
        --clean)    do_clean=1 ;;
        --no-cache) docker_build_flags+=("--no-cache") ;;
        --docs)
            echo "--docs is handled by build-linux.sh only; skip here" >&2
            ;;
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

# Host git state → handed into the cmake configure as a -D flag
# (matches build-linux.sh so the Linux + Windows binaries from the
# same branch report identical helper_caps.build_commit).
git_sha="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    git_sha="${git_sha}-dirty"
fi

# CMake generator: Ninja (NMake would work too; Ninja is faster on
# multi-TU builds and handles our FetchContent graph cleanly). The
# configure now targets /src (repo root) instead of /src/unio-pipe
# — top-level CMakeLists.txt drives both subdirectories.
# FETCHCONTENT_FULLY_DISCONNECTED isn't set here so first config
# clones msquic / libvpl (takes ~2 min; cached in /build volume).
CMAKE_CONFIGURE="CC=cl CXX=cl cmake -S /src -B /build \
    -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=AMD64 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TRY_COMPILE_CONFIGURATION=Release \
    -DCMAKE_POLICY_DEFAULT_CMP0141=NEW \
    -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded \
    -DPERL_EXECUTABLE=/usr/local/bin/winperl \
    -DUNIO_BUILD_COMMIT=$git_sha"

echo "=== 2/4  cmake configure (commit=$git_sha, toolchain=msvc-wine) ==="
# CONFIGURE_INSIST=1: msquic's vendored openssl3 Configure refuses
# to run under Linux Perl (its Configurations/windows-checker.pm
# rejects path-separator-mismatch). CONFIGURE_INSIST is OpenSSL's
# own documented escape hatch — the generated makefile has
# forward-slash paths, which nmake.exe under wine handles fine.
docker run --rm \
    -e CONFIGURE_INSIST=1 \
    -v "$REPO_ROOT:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "source /etc/msvc-env.sh && $CMAKE_CONFIGURE"

echo "=== 3/4  cmake build (targets: ${TARGETS[*]}) ==="
build_targets_csv=""
for t in "${TARGETS[@]}"; do
    build_targets_csv+="--target $t "
done
docker run --rm \
    -e CONFIGURE_INSIST=1 \
    -v "$REPO_ROOT:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "source /etc/msvc-env.sh && cmake --build /build -j \$(nproc) $build_targets_csv"

echo "=== 4/4  extract ==="
# Ship ONE file per target, nothing more. The MSVC C++ runtime is
# /MT so MSVCP140 / VCRUNTIME140 are NOT required on the target —
# verified via `dumpbin /DEPENDENTS`. For unio-pipe, Intel's real
# media runtime (libmfxhw64.dll) is still discovered + loaded at
# runtime by the oneVPL dispatcher code inside the exe, but that
# DLL ships with the user's Intel driver, not with us. unio-ui
# only needs Windows system DLLs (d3d11 / dxgi / user32 / gdi32).
#
# Binaries land in /build/bin/ thanks to CMAKE_RUNTIME_OUTPUT_DIRECTORY
# in the top-level CMakeLists.txt — each subdir doesn't need to
# know about output paths.
#
# --user $(id -u):$(id -g): without this, extracted files are
# owned by root (docker default) and require sudo to clear.
extract_cmd='set -e; mkdir -p /out'
for t in "${TARGETS[@]}"; do
    extract_cmd+="; cp /build/bin/$t.exe /out/$t.exe"
done
extract_cmd+="; echo 'commit: $git_sha' > /out/build-info.txt"
extract_cmd+="; echo \"built: \$(date -u +%Y-%m-%dT%H:%M:%SZ)\" >> /out/build-info.txt"
extract_cmd+="; echo \"image: $IMAGE_TAG\" >> /out/build-info.txt"
extract_cmd+="; echo 'targets: ${TARGETS[*]}' >> /out/build-info.txt"

docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$BUILD_VOLUME:/build:ro" \
    -v "$OUT_DIR:/out" \
    "$IMAGE_TAG" \
    "$extract_cmd"

echo
echo "=== DONE ==="
for t in "${TARGETS[@]}"; do
    echo "  binary:  $OUT_DIR/$t.exe"
    file "$OUT_DIR/$t.exe" || true
    ls -lah "$OUT_DIR/$t.exe"
done
cat "$OUT_DIR/build-info.txt"
