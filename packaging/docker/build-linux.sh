#!/bin/bash
# Build the repo's Linux binaries in a Docker container and emit
# them to dist/linux-x64/ on the host.
#
# Usage:
#   packaging/docker/build-linux.sh               # incremental build
#   packaging/docker/build-linux.sh --clean       # wipe build cache first
#   packaging/docker/build-linux.sh --no-cache    # force docker image rebuild
#
# Produces (subject to the UNIO_BUILD_* cmake options — all ON by default):
#   dist/linux-x64/unio-pipe          (ELF, media-path helper;
#                                      msquic + openssl3 static-linked,
#                                      single file)
#   dist/linux-x64/unio-ui            (ELF, UI layer; links X11 +
#                                      EGL + desktop GL dynamically —
#                                      distro system libs, same as
#                                      any Linux desktop app.
#                                      Sources live under unio-app/.)
#   dist/linux-x64/build-info.txt     (git commit + build timestamp)
#
# Implementation notes:
#   * Source tree is bind-mounted read-only. Image has no source
#     copy, so the same image builds any branch.
#   * Build artefacts live in a named Docker volume so cmake +
#     FetchContent don't redo multi-minute work on every run.
#   * Top-level CMakeLists.txt at the repo root is the build entry
#     point — see ARCHITECTURE.md §1 for the future-state single-
#     binary target.
#   * The git commit is computed on the host and handed into the
#     container via -DUNIO_BUILD_COMMIT=<sha>, matching the
#     matrix_test.py --sync convention so helper_caps.build_commit
#     reports the same value as a native host build would.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

IMAGE_TAG="unio-linux-builder:latest"
BUILD_VOLUME="unio-linux-build-cache"
OUT_DIR="$REPO_ROOT/dist/linux-x64"

# Targets produced by this script. Edit this list (and the
# UNIO_BUILD_* cmake options in the top-level CMakeLists.txt) when
# a new binary is added to the repo.
TARGETS=(unio-pipe unio-ui unio-license-tool)

do_clean=0
do_docs=0
docker_build_flags=()
for arg in "$@"; do
    case "$arg" in
        --clean)    do_clean=1 ;;
        --no-cache) docker_build_flags+=("--no-cache") ;;
        --docs)     do_docs=1 ;;
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
# (we could bind-mount it but that leaks arbitrary git hooks /
# refs into a sandbox we're trying to keep hermetic).
git_sha="$(git -C "$REPO_ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain 2>/dev/null)" ]]; then
    git_sha="${git_sha}-dirty"
fi

echo "=== 2/4  cmake configure (commit=$git_sha, toolchain=gcc-13) ==="
docker run --rm \
    -v "$REPO_ROOT:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "cmake -S /src -B /build \
        -DCMAKE_BUILD_TYPE=Release \
        -DUNIO_BUILD_COMMIT=$git_sha"

echo "=== 3/4  cmake build (targets: ${TARGETS[*]}) ==="
build_targets_csv=""
for t in "${TARGETS[@]}"; do
    build_targets_csv+="--target $t "
done
docker run --rm \
    -v "$REPO_ROOT:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    "$IMAGE_TAG" \
    "cmake --build /build -j \$(nproc) $build_targets_csv"

echo "=== 4/4  extract + strip ==="
# Ship ONE file per target, nothing more. Binaries land in
# /build/bin/ thanks to CMAKE_RUNTIME_OUTPUT_DIRECTORY in the
# top-level CMakeLists.txt — each target's subdir CMakeLists
# doesn't need to know about output paths.
#
# --user $(id -u):$(id -g): the extract step writes into the
# host's dist/ via a bind mount. Without this, files land owned
# by root (docker default), which tripped us up on unio-pipe
# early builds — a subsequent rm / re-run / scp out required sudo.
extract_cmd='mkdir -p /out'
for t in "${TARGETS[@]}"; do
    extract_cmd+=" && cp /build/bin/$t /out/$t && strip /out/$t"
done
# Ship unio-ui's third-party licence bundle (Inter OFL + ImGui MIT
# + stb_image) alongside the binary. Required by OFL §1.2 and
# MIT; a stand-alone text file satisfies both.
extract_cmd+=" && cp /src/unio-app/unio/LICENSES.txt /out/LICENSES.txt"
extract_cmd+=" && echo 'commit: $git_sha' > /out/build-info.txt"
extract_cmd+=" && echo \"built: \$(date -u +%Y-%m-%dT%H:%M:%SZ)\" >> /out/build-info.txt"
extract_cmd+=" && echo \"image: $IMAGE_TAG\" >> /out/build-info.txt"
extract_cmd+=" && echo 'targets: ${TARGETS[*]}' >> /out/build-info.txt"

docker run --rm \
    --user "$(id -u):$(id -g)" \
    -v "$REPO_ROOT:/src:ro" \
    -v "$BUILD_VOLUME:/build:ro" \
    -v "$OUT_DIR:/out" \
    "$IMAGE_TAG" \
    "$extract_cmd"

if (( do_docs )); then
    echo "=== extra  doxygen docs ==="
    # `docs` is an opt-in CMake target that runs doxygen against
    # the top-level Doxyfile. Two docker runs:
    #   1. Generate into /build/docs/html (root; matches the
    #      ownership of earlier configure/build steps)
    #   2. Copy /build/docs to /out/docs as the invoking user so
    #      the host's dist/ stays user-owned.
    docker run --rm \
        -v "$REPO_ROOT:/src:ro" \
        -v "$BUILD_VOLUME:/build" \
        "$IMAGE_TAG" \
        "cmake --build /build --target docs"
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -v "$BUILD_VOLUME:/build:ro" \
        -v "$OUT_DIR:/out" \
        "$IMAGE_TAG" \
        "rm -rf /out/docs && cp -r /build/docs /out/docs"
    echo "  docs:    $OUT_DIR/docs/html/index.html"
fi

echo
echo "=== DONE ==="
for t in "${TARGETS[@]}"; do
    echo "  binary:  $OUT_DIR/$t"
    file "$OUT_DIR/$t"
    ls -lah "$OUT_DIR/$t"
done
cat "$OUT_DIR/build-info.txt"
