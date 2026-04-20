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

a = Analysis(
    [str(ROOT / "scripts" / "launcher.py")],
    pathex=[str(ROOT)],
    binaries=[],
    datas=[
        (str(ROOT / "assets"), "assets"),
    ],
    hiddenimports=[
        "unio",
        "unio.apps",
        "unio.apps.peer",
        "unio.apps.shell",
        "unio.apps.layout_panel",
        "unio.apps.log_view",
        "unio.apps.source_overlay",
        "unio.apps.stream_window",
        "unio.apps.ui_theme",
        "unio.core",
        "unio.core.protocol",
        "unio.core.network",
        "unio.core.layout",
        "unio.core.discovery",
        "unio.features",
        "unio.features.capture_dxgi",
        "unio.features.capture_printwindow",
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
