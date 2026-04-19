# UnIO

A cross-platform distributed input routing system that makes multiple PCs behave like a single multi-monitor desktop. Move your mouse seamlessly across machines — even between Linux, Windows, and macOS — type on whichever screen has focus, and share your clipboard over the network.

**This is NOT a remote desktop.** Each machine renders its own display. UnIO only routes keyboard/mouse input and synchronizes clipboard content.

## Highlights

- Single-window desktop app (Activity · Layout · Settings) — no CLI needed.
- LAN discovery so joining is "pick a host from the list", or enter an IP manually.
- Drag-and-drop display layout with zoom, fit, and a safety check that blocks layouts where a monitor would be isolated from the rest.
- Identify overlay numbers each physical screen; numbering is grouped per computer and consistent everywhere.
- Every connected computer drives the shared cursor + keyboard equally — no "source" to hand off.
- Per-computer **Input** and **Clipboard** toggles in the Activity tab, synced across every PC via the server.
- A muted computer keeps local keyboard + mouse working normally; only when the shared cursor lands on it does local input step aside for the remote controller.
- Clipboard sync works in both directions; toggling a computer off excludes it from both send and receive.
- Server auto-times-out clients that stop responding to heartbeats, and wipes the layout when the host is alone so the canvas never shows ghost displays.
- Linux builds ship as both a raw ELF binary and an AppImage (carries the UnIO icon + `.desktop` entry). Windows ships as a onedir bundle; macOS as a single binary.

## Architecture

```
┌─────────────┐     TCP      ┌─────────────┐     TCP      ┌─────────────┐
│   Client A  │◄────────────►│   Server    │◄────────────►│   Client B  │
│  (PC1)      │              │  (Layout +  │              │  (PC2)      │
│             │              │   Router)   │              │             │
│ ┌─────────┐ │              │ ┌─────────┐ │              │ ┌─────────┐ │
│ │ Monitor │ │              │ │ Layout  │ │              │ │ Monitor │ │
│ │ 1920x1080│ │              │ │ Manager │ │              │ │1920x1080│ │
│ └─────────┘ │              │ └─────────┘ │              │ └─────────┘ │
└─────────────┘              └─────────────┘              └─────────────┘

Global coordinate space:
[  0,0 ──── 1920,0 ──── 3840,0  ]
[  PC1 Monitor      PC2 Monitor  ]
```

### Components

| Component | File | Role |
|-----------|------|------|
| Launcher | `scripts/launcher.py` | Thin entry — forwards to the shell |
| Shell | `unio/apps/shell.py` | Single-window UI: Activity / Layout / Settings, session lifecycle |
| Layout Panel | `unio/apps/layout_panel.py` | Drag-and-drop display canvas with zoom + isolation check |
| Protocol | `unio/core/protocol.py` | Wire format, message types, serialization |
| Network Layer | `unio/core/network.py` | Async TCP with framed messages |
| Discovery | `unio/core/discovery.py` | UDP LAN announce + scan (port 24801) |
| Layout Manager | `unio/core/layout.py` | Global coordinate space, edge detection, handoff computation |
| Server | `unio/apps/server.py` | Central coordinator: layout, routing, heartbeat watchdog, per-machine toggles |
| Client | `unio/apps/client.py` | Per-machine agent: capture, inject, edge detect, clipboard poll |
| Web UI | `unio/apps/webui.py` | HTTP server + single-page app (alternative surface) |
| UI theme | `unio/apps/ui_theme.py` | Shared Tk styling (pill buttons, rail, icons) |
| File Transfer | `unio/features/filetransfer.py` | Send files between machines |
| Identify Overlay | `unio/features/identify.py` | Fallback multi-process overlay (shell now renders Toplevels in-process) |
| **Platform Backends** | `unio/backends/` | **OS-specific input capture, injection, clipboard, OS display config** |
| &nbsp;&nbsp;Abstract Interface | `unio/backends/__init__.py` | `InputBackend` ABC + auto-detection |
| &nbsp;&nbsp;Linux/X11 | `unio/backends/linux_x11.py` | XTest, Xinerama, evdev, xclip, xrandr |
| &nbsp;&nbsp;Windows | `unio/backends/windows.py` | SendInput, low-level hooks, Win32 clipboard, ChangeDisplaySettingsEx |
| &nbsp;&nbsp;macOS | `unio/backends/macos.py` | Quartz Event Services, CGEventTap, pbcopy, CGDisplayConfigure |
| &nbsp;&nbsp;Keycodes | `unio/backends/keycodes.py` | USB HID ↔ native keycode mapping |
| Assets | `assets/` | App icon (`logo_*.png`, `logo_mark_*.png`, `logo_mark.ico`) |
| Packaging | `packaging/` | PyInstaller spec, build script, `.desktop` entry, AppImage wrapper |

### Cross-platform keycode translation

All keyboard events use **USB HID scan codes** on the wire. Each backend converts between native keycodes and HID at the OS boundary, so a Linux machine can seamlessly send keystrokes to a Windows or macOS machine.

```
Linux (evdev 30) → HID 0x04 (A) → wire → HID 0x04 → Windows (VK 0x41)
                                                    → macOS (kVK 0x00)
```

## Requirements

The pre-built binaries bundle their own Python interpreter, so the
only thing you need on the host machine is the OS-level libraries
used for input capture.

- **Linux (X11):** `libx11 libxtst libxinerama xrandr xclip`, plus
  membership in the `input` group for keyboard capture
  (`sudo usermod -aG input $USER`, then log out / back in).
- **Windows:** nothing extra; uses Win32 API via ctypes. Run
  elevated if you need to inject input into elevated apps.
- **macOS:** grant *Accessibility* to the UnIO app in
  *System Settings → Privacy & Security → Accessibility* — without
  it, cursor warp and input capture fail silently.

### Running from source

Python 3.10+ and:

```bash
pip install -r requirements.txt
```

(requirements.txt pulls `pyyaml` and `Pillow`.)

## Quick Start

### Install

Grab a pre-built binary for your OS from the
[Releases page](https://github.com/4d1-1010/UnIO/releases):

```bash
# Linux — raw binary or AppImage (the AppImage ships the UnIO icon
# + .desktop entry, which is what file managers / docks will render)
tar xzf unio-*-linux-x64.tar.gz && ./unio
# or
chmod +x UnIO-*-x86_64.AppImage && ./UnIO-*-x86_64.AppImage

# macOS
tar xzf unio-*-macos-arm64.tar.gz
xattr -cr unio        # clear Gatekeeper quarantine
./unio

# Windows — unzip unio-*-windows-x64.zip, run unio\unio.exe
```

No Python install or extra dependencies required. `latest` is a rolling
pre-release from `main`; tagged `v*` releases are immutable.

### Run

> **Linux first-time setup (required):** keyboard capture reads
> `/dev/input/event*`, which is only accessible to the `input` group.
> Add yourself once and log out / back in:
>
> ```bash
> sudo usermod -aG input $USER
> ```
>
> Without this the cursor will still cross, but typed keys won't
> forward — UnIO logs `Keyboard capture disabled` when it hits this.

1. On the machine you want to use as the hub, launch UnIO and pick
   **"Start hosting"**. The Activity tab shows your LAN IP and port
   (e.g. `192.168.1.10:24800`) — share that with the other machines.
2. On the other machine(s), launch UnIO and pick **"Find hosts"** —
   UnIO scans the LAN, lists every host that answered, double-click
   one to connect. (Or enter an address manually from that dialog.)
3. Open the **Layout** tab on any connected machine. Drag each
   machine's monitors until they match the physical arrangement,
   then click **Apply layout**. UnIO reconfigures the OS display
   positions on every machine so the cursor crosses at the seams you
   defined.

Now move your mouse to any shared edge — the cursor hops to the next
machine. Keyboard input follows the cursor, and copied text syncs
across the network.

### Activity tab

The Activity tab lists every connected computer with its OS. For each
one you get two toggles:

- **Input · ON/OFF** — when OFF, the server drops that computer's
  keyboard + mouse events. The muted computer still works locally as
  normal, until the shared cursor lands on it (another peer pushed it
  over), at which point local input steps aside so the remote user
  can work cleanly. As soon as the cursor leaves, local operation
  resumes.
- **Clipboard · ON/OFF** — when OFF, that computer is excluded from
  clipboard sync in both directions.

Both toggles live on the server and are broadcast to every connected
shell, so flipping one on any PC is reflected instantly on every
other PC. Defaults are ON for every newly-joined computer.

### Layout tab

The Layout canvas renders every connected computer's monitors grouped
by machine, numbered left-to-right, and tinted with a per-machine
color that stays consistent across every PC.

- **Drag** a monitor to reposition it. Edges snap to neighbours.
- **Mouse wheel** zooms (around the pointer). The `−` / `Fit` / `+`
  buttons do the same from the keyboard / touch path.
- **Identify displays** flashes a numbered overlay on every physical
  screen so you know which rectangle is which.
- **Apply layout** pushes the arrangement to every PC. The button
  refuses to apply if any display would be isolated — every monitor
  must share at least 1/3 of an edge with another monitor so the
  cursor can actually cross.

The layout is saved to `layout.json` next to the server and restored
on reconnect.

## Run from source

If you want to hack on UnIO or skip the binary download:

```bash
pip install -r requirements.txt
python scripts/launcher.py
```

Headless / scripted entry points are still available for automation:

```bash
python scripts/run_server.py       # server only
python scripts/run_client.py --id pc1 --server SERVER_IP
```

The old `run_configurator.py` still exists for the legacy standalone
window but the shell has replaced it as the primary UI.

### Building a standalone binary

See `packaging/README.md`. Short version:

```bash
pip install pyinstaller
python packaging/build.py --clean
# → Linux:   dist/unio  +  dist/UnIO-x86_64.AppImage (if appimagetool on PATH)
#   macOS:   dist/unio
#   Windows: dist/unio/unio.exe  (onedir, Defender-friendly)
```

Linux AppImage step requires
[appimagetool](https://github.com/AppImage/AppImageKit/releases) on
`$PATH`; drop it in `~/.local/bin` (no sudo). If it's missing the
script just skips the AppImage and produces the raw ELF.

CI at `.github/workflows/build.yml` builds all three OSes (Linux
installs `libfuse2` + `appimagetool` automatically) on every push to
`main` and attaches them to the rolling `latest` release; the
*Cut a versioned release* workflow (in the Actions tab) produces
immutable `v*` releases.

## File Transfer

Send a file from one machine to another:

```bash
python scripts/send_file.py --server SERVER_IP --from pc1 --to pc2 --file /path/to/file.pdf
```

Files are saved to `~/UnIO-Received/` on the target machine.

## Running Tests

```bash
python tests/test_core.py        # protocol + layout unit tests
python tests/test_network.py     # client/server integration over TCP
python tests/test_multimon.py    # multi-monitor edge cases
python tests/test_keycodes.py    # HID ↔ native keycode mapping
python tests/test_webui.py       # web UI API tests
```

## Configuration Reference

The launcher handles layout interactively, but you can also pre-seed
it via `config.yaml` next to the server:

```yaml
server:
  host: "0.0.0.0"      # bind address
  port: 24800          # TCP port

layout:
  machines:
    machine_id:        # must match the --id the client registers with
      offset_x: 0      # global X origin for this machine's monitors
      offset_y: 0      # global Y origin
```

If the file is missing or a machine isn't listed, monitors are
auto-placed left-to-right in connection order. The configurator's
*Apply Layout* always wins over this file.

### Vertical / L-shaped layouts

```yaml
layout:
  machines:
    top_pc:
      offset_x: 0
      offset_y: 0
    bottom_pc:
      offset_x: 0
      offset_y: 1080    # stacked below the top PC
```

## Protocol

Binary framed, little-endian:

```
┌──────────────────┬────────────────────┬──────────────────┐
│ type (uint16)    │ length (uint32)    │ payload (JSON)   │
│ 2 bytes          │ 4 bytes            │ variable         │
└──────────────────┴────────────────────┴──────────────────┘
```

### Message types

| Type | Code | Direction | Description |
|------|------|-----------|-------------|
| REGISTER | 0x01 | C→S | Client registers with machine ID + monitors |
| REGISTER_ACK | 0x02 | S→C | Registration confirmation |
| LAYOUT_UPDATE | 0x03 | S→C | Broadcast global layout to all clients |
| LAYOUT_APPLY | 0x04 | C→S | Configurator pushes new display positions |
| APPLY_MONITORS | 0x05 | S→C | Instruct client to reconfigure its OS displays |
| MOUSE_MOVE_ABS | 0x10 | C→S→C | Absolute mouse position (global coords) |
| MOUSE_MOVE_REL | 0x11 | C→S→C | Relative mouse delta |
| MOUSE_BUTTON | 0x12 | C→S→C | Mouse button press/release |
| MOUSE_SCROLL | 0x13 | C→S→C | Scroll wheel |
| KEY_EVENT | 0x14 | C→S→C | Keyboard press/release |
| EDGE_HIT | 0x20 | C→S | Cursor reached screen edge |
| HANDOFF | 0x21 | S→C | Cursor ownership handoff to target machine |
| ACTIVATE | 0x22 | S→C | You now own the cursor |
| DEACTIVATE | 0x23 | S→C | Release cursor, start forwarding |
| CLAIM_FOCUS | 0x24 | C→S | Dormant client's mouse moved — take focus |
| SET_INPUT_SOURCE | 0x25 | — | *Deprecated* (make-source feature removed) |
| INPUT_SOURCE_STATE | 0x26 | — | *Deprecated* (always true now) |
| SET_INPUT_MUTED | 0x27 | C→S | Toggle a computer's keyboard+mouse on/off |
| SET_CLIPBOARD_SYNC | 0x28 | C→S | Toggle a computer's clipboard sync on/off |
| CLIPBOARD_UPDATE | 0x30 | C→S→C | Clipboard content changed |
| FILE_OFFER | 0x40 | C→S→C | File transfer initiation |
| FILE_ACCEPT | 0x41 | C→S→C | Receiver accepts file offer |
| FILE_CHUNK | 0x42 | C→S→C | File data chunk |
| FILE_DONE | 0x43 | C→S→C | File transfer complete |
| IDENTIFY | 0x50 | S→C | Show numbered overlay on each monitor |
| IDENTIFY_ACK | 0x51 | C→S | Identify overlay displayed |
| REQUEST_IDENTIFY | 0x52 | C→S | Configurator asks server to trigger IDENTIFY |
| HEARTBEAT | 0xF0 | S↔C | Keep-alive ping |
| HEARTBEAT_ACK | 0xF1 | S↔C | Keep-alive reply |

## How Cursor Handoff Works

1. **Active machine** polls cursor position at 120Hz
2. When cursor reaches a screen edge, check the global layout for an adjacent monitor on a different machine
3. If found, send `EDGE_HIT` to the server
4. Server computes the entry point on the target monitor
5. Server sends `DEACTIVATE` to the source, `ACTIVATE` to the target
6. Source grabs pointer+keyboard and starts forwarding input as relative deltas
7. Target warps cursor to entry point and injects received events via platform backend
8. When the cursor later hits an edge back, the process reverses

## Troubleshooting

### Linux

**"Cannot open X display"**
- Ensure `$DISPLAY` is set (usually `:0` or `:1`)
- Run from within an X11 session (not pure Wayland)

**"No keyboard input devices found"**
- Add your user to the `input` group: `sudo usermod -aG input $USER`
- Log out and back in

**Clipboard not syncing**
- Install `xclip`: `sudo apt install xclip`

### Windows

**Input injection not working in some apps**
- Run from an Administrator command prompt
- Some apps with elevated privileges reject injected input from non-elevated processes

**Keyboard not captured**
- The low-level hook requires a message pump — this runs automatically in a background thread

### macOS

**"Failed to create CGEventTap"**
- Grant Accessibility permissions: System Settings → Privacy & Security → Accessibility
- Add your terminal app (Terminal.app, iTerm2, etc.) to the list

**Cursor not moving**
- Accessibility permissions are also required for `CGWarpMouseCursorPosition`

### All platforms

**Cursor doesn't cross to the other machine**
- Open the host window and use *Apply Layout* to align monitors at their shared edge (or edit `config.yaml` if running headless)
- Ensure all machines show up in the configurator's sidebar
- Launch the server with `-v` for debug logging

**High latency**
- Ensure machines are on the same LAN
- The server should ideally run on the same machine as one of the clients

## Security Notes

- Traffic is **unencrypted**. Input events (including keystrokes) are sent in plaintext.
- For production use, tunnel through SSH: `ssh -L 24800:localhost:24800 server-host`
- Only run on trusted networks.

## Limitations

- Linux: X11 only (no Wayland yet)
- Linux: keyboard forwarding requires `input` group membership
- macOS: requires Accessibility permissions
- No encryption (use SSH tunneling)
- No drag-and-drop file transfer (CLI only)
- No per-application hotkey passthrough
- Multi-user model is collaborative: every unmuted PC drives the
  shared cursor simultaneously. Use the per-computer **Input**
  toggle in the Activity tab to isolate a specific machine.
