#!/bin/bash
# Build the Windows unio-pipe.exe via msvc-wine in a Docker
# container and emit it to dist/win-x64/unio-pipe.exe on the host.
#
# Runs entirely on the Linux orchestrator — no Windows host
# required. msvc-wine runs Microsoft's real MSVC toolchain under
# wine, so the output PE32+ binary is bit-identical to a VS 2022
# build on a native Windows machine.
#
# Usage:
#   packaging/docker/build-win.sh              # incremental
#   packaging/docker/build-win.sh --clean      # wipe build volume
#   packaging/docker/build-win.sh --no-cache   # force image rebuild
#
# Produces:
#   dist/win-x64/unio-pipe.exe       (PE32+, MSVC 17.x, NOT stripped;
#                                     msquic + openssl3 statically linked,
#                                     /MT for the MSVC CRT — no DLLs
#                                     shipped beside it unless a later
#                                     optional component like libvpl.dll
#                                     adds one back)
#   dist/win-x64/*.dll               (only if an optional component ships
#                                     one — e.g. libvpl.dll when #49's
#                                     oneVPL path lands)
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

IMAGE_TAG="unio-pipe-win-builder:latest"
BUILD_VOLUME="unio-pipe-win-build-cache"
OUT_DIR="$REPO_ROOT/dist/win-x64"

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

# Host git state → handed into the cmake configure as a -D flag
# (matches Dockerfile.linux's build-linux.sh convention so a
# Linux binary and a Windows binary from the same branch report
# identical helper_caps.build_commit).
git_sha="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    git_sha="${git_sha}-dirty"
fi

# CMake generator: msvc-wine works with the NMake or Ninja
# generators. NMake is in-tree (no extra install); keep it simple.
# FETCHCONTENT_FULLY_DISCONNECTED isn't set here so first config
# clones msquic / libvpl (takes ~2 min; cached in /build volume).
CMAKE_CONFIGURE="CC=cl CXX=cl cmake -S /src/unio-pipe -B /build \
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

echo "=== 3/4  cmake build ==="
docker run --rm \
    -e CONFIGURE_INSIST=1 \
    -v "$REPO_ROOT:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "source /etc/msvc-env.sh && cmake --build /build -j \$(nproc) --target unio-pipe"

echo "=== 4/4  extract ==="
# Copy the EXE + any optional runtime DLLs we still ship:
#   libvpl.dll   — Intel oneVPL dispatcher (post-#49 Windows
#                  Intel path; absent on main, harmless if
#                  missing — glob matches nothing).
#
# msquic + openssl3 are statically linked into unio-pipe.exe
# (CMakeLists: QUIC_BUILD_SHARED=OFF). The MSVC C++ runtime is
# also static (/MT) so MSVCP140 / VCRUNTIME140 are NOT required
# on the target — verified via PE import scan. A minimal ship
# from main is therefore a single unio-pipe.exe.
# --user $(id -u):$(id -g): without this, extracted files are
# owned by root (docker default) and require sudo to clear. See
# build-linux.sh for the same fix.
docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$BUILD_VOLUME:/build:ro" \
    -v "$OUT_DIR:/out" \
    "$IMAGE_TAG" \
    "set -e; \
     cp /build/Release/unio-pipe.exe /out/ 2>/dev/null || cp /build/unio-pipe.exe /out/; \
     declare -A seen; \
     for dll in \$(find /build -maxdepth 6 -name 'libvpl.dll' 2>/dev/null); do \
         base=\$(basename \"\$dll\"); \
         if [[ -z \"\${seen[\$base]:-}\" ]]; then \
             cp \"\$dll\" /out/; \
             seen[\$base]=1; \
         fi; \
     done; \
     echo 'commit: $git_sha' > /out/build-info.txt; \
     echo \"built: \$(date -u +%Y-%m-%dT%H:%M:%SZ)\" >> /out/build-info.txt; \
     echo \"image: $IMAGE_TAG\" >> /out/build-info.txt"

echo
echo "=== DONE ==="
echo "  binary:  $OUT_DIR/unio-pipe.exe"
file "$OUT_DIR/unio-pipe.exe" || true
ls -lah "$OUT_DIR/"
cat "$OUT_DIR/build-info.txt"
