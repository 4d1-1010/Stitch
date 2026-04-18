# Packaging

This directory contains everything needed to produce a standalone
distributable binary of UnIO.

## Requirements

```bash
pip install pyinstaller
pip install -r ../requirements.txt   # pyyaml, Pillow
```

## Building

Run on **each target OS** — PyInstaller does not cross-compile.

```bash
# From the repo root:
python packaging/build.py            # fresh build into dist/
python packaging/build.py --clean    # wipe dist/ + build/ first
python packaging/build.py --app      # also produce dist/unio.app (macOS)
```

### What gets produced

| OS      | Output                         |
| ------- | ------------------------------ |
| Linux   | `dist/unio` (~13 MB ELF)     |
| Windows | `dist/unio.exe`              |
| macOS   | `dist/unio` and/or `.app`    |

The binary is self-contained: it embeds the Python interpreter,
`unio/` package, `assets/` (icons), and the PyYAML + Pillow
dependencies. No Python install is required on the target machine.

### Platform notes

**Linux** — the binary still links against the running system's X11,
Xinerama, and xrandr. Users need:

```bash
sudo apt install libx11-6 libxtst6 libxinerama1 xrandr xclip
```

(or the equivalent on their distro).

**Windows** — the spec sets `console=False` so launching doesn't open
a terminal window. Elevation may be required for input injection into
elevated apps.

**macOS** — first run prompts the user to grant *Accessibility* under
System Settings → Privacy & Security. Without it, cursor warp and
input capture fail silently.

## Continuous builds (free)

`.github/workflows/build.yml` builds all three binaries on push to `main`
and uploads them as artifacts. Pushing a `v*` tag also publishes them as
a GitHub release.

Runners used: `ubuntu-latest`, `windows-latest`, `macos-latest` — all free
on public repos, generous quota on private.

## Desktop integration (Linux)

`packaging/unio.desktop` is a launcher entry for GNOME / KDE.
Update the `Exec=` and `Icon=` paths if you install the binary
somewhere other than `/opt/unio/`, then:

```bash
sudo install -Dm644 packaging/unio.desktop /usr/share/applications/unio.desktop
sudo install -Dm644 assets/logo_256.png      /usr/share/icons/hicolor/256x256/apps/unio.png
```
