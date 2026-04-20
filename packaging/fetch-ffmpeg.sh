#!/usr/bin/env bash
# Download LGPL-licensed static FFmpeg builds for the platforms unIO
# ships on, and drop them under vendor/ffmpeg/<os-arch>/ffmpeg(.exe).
#
# Source: BtbN's builds at https://github.com/BtbN/FFmpeg-Builds —
# the "lgpl" variant has no libx264 / no libfdk-aac / no GPL-only
# libs, so it's safe to redistribute under LGPL 2.1+ inside a
# closed-source commercial app (with the usual LGPL notice +
# written offer / upstream-source link).
#
# The hardware H.264 encoders we actually want (NVENC, Quick Sync,
# AMF, VAAPI, VideoToolbox) are built on top of vendor SDKs, not
# libx264, and the LGPL build includes all of them.
#
# Run this before packaging/build.py so the PyInstaller spec can
# pick the binary up from `vendor/ffmpeg/…`.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VENDOR_DIR="${REPO_ROOT}/vendor/ffmpeg"
VERSION="${UNIO_FFMPEG_VERSION:-master-latest}"
BASE_URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/latest"

mkdir -p "${VENDOR_DIR}"

fetch_linux_x86_64() {
    local dest="${VENDOR_DIR}/linux-x86_64"
    mkdir -p "${dest}"
    if [[ -x "${dest}/ffmpeg" ]] && [[ "${UNIO_FFMPEG_FORCE:-}" != "1" ]]; then
        echo "[ffmpeg] linux-x86_64 already present, skipping"
        return
    fi
    local tar="${dest}/ffmpeg.tar.xz"
    echo "[ffmpeg] fetching linux-x86_64 LGPL…"
    curl -fL --retry 3 --retry-delay 3 \
        "${BASE_URL}/ffmpeg-${VERSION}-linux64-lgpl.tar.xz" -o "${tar}"
    tar -xJf "${tar}" -C "${dest}" --strip-components=2 \
        "$(tar -tJf "${tar}" | grep -m1 '/bin/ffmpeg$')"
    rm -f "${tar}"
    chmod +x "${dest}/ffmpeg"
    echo "[ffmpeg] linux-x86_64 → ${dest}/ffmpeg"
}

fetch_windows_x86_64() {
    local dest="${VENDOR_DIR}/windows-x86_64"
    mkdir -p "${dest}"
    if [[ -x "${dest}/ffmpeg.exe" ]] && [[ "${UNIO_FFMPEG_FORCE:-}" != "1" ]]; then
        echo "[ffmpeg] windows-x86_64 already present, skipping"
        return
    fi
    local zip="${dest}/ffmpeg.zip"
    echo "[ffmpeg] fetching windows-x86_64 LGPL…"
    curl -fL --retry 3 --retry-delay 3 \
        "${BASE_URL}/ffmpeg-${VERSION}-win64-lgpl.zip" -o "${zip}"
    # unzip into a temp dir because the archive ships a single
    # versioned top-level folder that we don't want to encode into
    # the bundle path.
    local tmp
    tmp="$(mktemp -d)"
    unzip -q "${zip}" -d "${tmp}"
    cp "${tmp}"/*/bin/ffmpeg.exe "${dest}/ffmpeg.exe"
    rm -rf "${tmp}" "${zip}"
    echo "[ffmpeg] windows-x86_64 → ${dest}/ffmpeg.exe"
}

fetch_linux_x86_64
fetch_windows_x86_64

# Copy LICENSE files next to each binary so the LGPL notice travels
# with the artefact.
for os_dir in "${VENDOR_DIR}/linux-x86_64" "${VENDOR_DIR}/windows-x86_64"; do
    if [[ ! -f "${os_dir}/LICENSE.txt" ]]; then
        cat > "${os_dir}/LICENSE.txt" <<'EOF'
This directory contains a build of FFmpeg (https://ffmpeg.org)
redistributed under the GNU Lesser General Public License version
2.1 or later.

Source code for the corresponding FFmpeg release is available at:
    https://github.com/FFmpeg/FFmpeg
    https://github.com/BtbN/FFmpeg-Builds

No GPL-only components (libx264, libx265, libfdk-aac, etc.) are
included in this build. Hardware encoders (NVENC, Intel Quick
Sync, AMD AMF, VA-API, VideoToolbox) rely on vendor SDKs licensed
separately by the hardware manufacturer.

The full LGPL-2.1 text is available at:
    https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
EOF
    fi
done

echo "[ffmpeg] done."
