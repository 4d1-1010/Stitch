# PyInstaller spec for Stitch.
#
# Linux + macOS   → single-file binary (dist/stitch)
# Windows         → onedir bundle      (dist/stitch/stitch.exe + _internal/)
#
# Onedir on Windows avoids the "self-extracting archive" heuristic that
# makes Defender flag single-file PyInstaller .exe files as malware.
#
# Run from the repo root:
#     pyinstaller packaging/stitch.spec

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
        "stitch",
        "stitch.apps",
        "stitch.apps.server",
        "stitch.apps.client",
        "stitch.apps.configurator",
        "stitch.apps.webui",
        "stitch.apps.ui_theme",
        "stitch.core",
        "stitch.core.protocol",
        "stitch.core.network",
        "stitch.core.layout",
        "stitch.features",
        "stitch.features.filetransfer",
        "stitch.features.identify",
        "stitch.backends",
        "stitch.backends.linux_x11",
        "stitch.backends.windows",
        "stitch.backends.macos",
        "stitch.backends.keycodes",
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
        name="stitch",
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
        icon=str(ROOT / "assets" / "logo_256.png"),
    )
else:
    exe = EXE(
        pyz,
        a.scripts,
        [],
        exclude_binaries=True,
        name="stitch",
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
        icon=str(ROOT / "assets" / "logo_256.png"),
    )
    coll = COLLECT(
        exe,
        a.binaries,
        a.zipfiles,
        a.datas,
        strip=False,
        upx=False,
        upx_exclude=[],
        name="stitch",
    )
