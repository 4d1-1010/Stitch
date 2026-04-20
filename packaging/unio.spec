# PyInstaller spec for unIO.
#
# Linux + macOS   → single-file binary (dist/unio)
# Windows         → onedir bundle      (dist/unio/unio.exe + _internal/)
#
# Onedir on Windows avoids the "self-extracting archive" heuristic that
# makes Defender flag single-file PyInstaller .exe files as malware.
#
# Run from the repo root:
#     pyinstaller packaging/unio.spec

import sys
from pathlib import Path

ROOT = Path(SPECPATH).parent.resolve()
sys.path.insert(0, str(ROOT))

ONEFILE = sys.platform != "win32"

block_cipher = None

# Bundled FFmpeg (LGPL build, hardware encoders + libopenh264). The
# vendor drop is populated by packaging/fetch-ffmpeg.sh; if it's
# missing at build time we warn and continue without H.264 support
# so a dev build doesn't hard-fail when someone forgot the step.
def _ffmpeg_bundle_entries():
    os_arch = ("windows-x86_64" if sys.platform == "win32"
               else "linux-x86_64")
    exe = "ffmpeg.exe" if sys.platform == "win32" else "ffmpeg"
    src = ROOT / "vendor" / "ffmpeg" / os_arch / exe
    license_src = ROOT / "vendor" / "ffmpeg" / os_arch / "LICENSE.txt"
    out = []
    if src.exists():
        out.append((str(src), "ffmpeg"))
        if license_src.exists():
            out.append((str(license_src), "ffmpeg"))
    else:
        print(f"WARNING: vendor ffmpeg missing: {src} — "
              "run packaging/fetch-ffmpeg.sh to enable H.264",
              file=sys.stderr)
    return out


_FFMPEG_ENTRIES = _ffmpeg_bundle_entries()

a = Analysis(
    [str(ROOT / "scripts" / "launcher.py")],
    pathex=[str(ROOT)],
    binaries=[e for e in _FFMPEG_ENTRIES
              if not e[0].endswith("LICENSE.txt")],
    datas=[
        (str(ROOT / "assets"), "assets"),
    ] + [e for e in _FFMPEG_ENTRIES
         if e[0].endswith("LICENSE.txt")],
    hiddenimports=[
        "unio",
        "unio.apps",
        "unio.apps.peer",
        "unio.apps.shell",
        "unio.apps.layout_panel",
        "unio.apps.log_view",
        "unio.apps.source_overlay",
        "unio.apps.stream_window",
        "unio.apps.stream_window_win32_native",
        "unio.apps.ui_theme",
        "unio.core",
        "unio.core.protocol",
        "unio.core.network",
        "unio.core.layout",
        "unio.core.discovery",
        "unio.features",
        "unio.features.capture_dxgi",
        "unio.features.capture_printwindow",
        "unio.features.capture_windows_bitblt",
        "unio.features.capture_windows_wgc",
        "unio.features.capture_windows_mss",
        "unio.features.capture_windows_perwindow",
        "unio.features.capture_xcomposite",
        "unio.features.display_stream",
        "unio.features.filetransfer",
        "unio.features.frame_delta",
        "unio.features.hw_pipeline",
        "unio.features.identify",
        "unio.features.virtual_display",
        "mss",
        "unio.backends",
        "unio.backends.linux_x11",
        "unio.backends.windows",
        "unio.backends.macos",
        "unio.backends.keycodes",
        "PIL._tkinter_finder",
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

if ONEFILE:
    exe = EXE(
        pyz,
        a.scripts,
        a.binaries,
        a.zipfiles,
        a.datas,
        [],
        name="unio",
        debug=False,
        bootloader_ignore_signals=False,
        strip=False,
        upx=False,
        runtime_tmpdir=None,
        console=False,
        disable_windowed_traceback=False,
        argv_emulation=False,
        target_arch=None,
        codesign_identity=None,
        entitlements_file=None,
        icon=str(ROOT / "assets" / (
            "logo_mark.ico" if sys.platform == "win32"
            else "logo_mark_256.png"
        )),
    )
else:
    exe = EXE(
        pyz,
        a.scripts,
        [],
        exclude_binaries=True,
        name="unio",
        debug=False,
        bootloader_ignore_signals=False,
        strip=False,
        upx=False,
        console=False,
        disable_windowed_traceback=False,
        argv_emulation=False,
        target_arch=None,
        codesign_identity=None,
        entitlements_file=None,
        icon=str(ROOT / "assets" / (
            "logo_mark.ico" if sys.platform == "win32"
            else "logo_mark_256.png"
        )),
    )
    coll = COLLECT(
        exe,
        a.binaries,
        a.zipfiles,
        a.datas,
        strip=False,
        upx=False,
        upx_exclude=[],
        name="unio",
    )
