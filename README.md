# unIO

A cross-platform distributed input routing system that makes multiple PCs behave like a single multi-monitor desktop. Move your mouse seamlessly across machines — even between Linux, Windows, and macOS — type on whichever screen has focus, and share your clipboard over the network.

**This is NOT a remote desktop.** Each machine renders its own display. unIO only routes keyboard/mouse input and synchronizes clipboard content.

## Highlights

- **Full-mesh P2P** — every PC runs the same `Peer` module on launch. Peers auto-discover each other via UDP broadcast on every local interface (cable + WiFi simultaneously), open direct TCP connections in a full mesh, and replicate shared state via Last-Writer-Wins gossip. No host / client distinction; any PC dropping off doesn't take the rest down.
- Single-window desktop app (Activity · Layout · Settings), nothing to configure.
- Drag-and-drop display layout with zoom, fit, and a safety check that blocks layouts where a monitor would be isolated from the rest. An Apply propagates via LWW gossip so every peer sees the same arrangement.
- Identify overlay numbers each physical screen; numbering is grouped per computer and consistent on every PC.
- Every connected computer drives the shared cursor + keyboard equally — two people collaborating just pick up each other's mouse inputs.
- Per-computer **Input** and **Clipboard** toggles in the Activity tab, replicated to every peer instantly.
- A muted computer keeps local keyboard + mouse working normally; only when the shared cursor lands on it does local input step aside for the remote controller.
- Clipboard sync is two-way and mesh-aware (the server-side filter is now per-peer).
- Heartbeat watchdog on every mesh link evicts stale peers in 15 s so dropped laptops don't leave ghost monitors on everyone's canvas.
- Linux builds ship as both a raw ELF binary and an AppImage (carries the unIO icon + `.desktop` entry). Windows ships as a onedir bundle; macOS as a single binary.

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
| Shell | `unio/apps/shell.py` | Single-window UI: Activity / Layout / Settings, owns the Peer |
| Peer | `unio/apps/peer.py` | Full-mesh P2P node — listener + dialer + LWW state + input routing + clipboard + cursor handoff |
| Layout Panel | `unio/apps/layout_panel.py` | Drag-and-drop display canvas with zoom + isolation check |
| Protocol | `unio/core/protocol.py` | Wire format, message types, serialization |
| Network Layer | `unio/core/network.py` | Async TCP with framed messages |
| Discovery | `unio/core/discovery.py` | UDP mesh announce + legacy probe/reply (port 24801), multi-interface |
| Layout Manager | `unio/core/layout.py` | Global coordinate space, edge detection, handoff computation |
| UI theme | `unio/apps/ui_theme.py` | Shared Tk styling (pill buttons, rail, icons) |
| File Transfer | `unio/features/filetransfer.py` | Send files between peers (CLI) |
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

unIO has **three tiers** of requirements: core (always needed),
streaming (needed for display-share / virtual displays), and virtual
displays (needed for creating phantom monitors). Each tier can be
skipped — the feature using it just turns off cleanly.

### Tier 1 — Core (always required)

Everything in this tier ships inside the pre-built binaries except
OS-level system libraries.

#### Linux

```bash
# Runtime libraries (most distros already have these):
sudo apt install libx11-6 libxtst6 libxinerama1 xrandr xclip

# Keyboard capture needs membership in the `input` group. One time:
sudo usermod -aG input $USER
# then log out / back in for the group change to take effect.
```

**Why sudo once**: `/dev/input/event*` is root-only by default;
adding yourself to the `input` group grants read access so unIO can
capture keyboard events. Without this, cursor crossing works but
typed keys don't forward.

#### Windows

Nothing extra. Input capture goes through Win32 (`SendInput`, low-
level hooks, `GetAsyncKeyState`) via ctypes — no external DLLs.

**When you need admin / elevation**: if you want unIO to inject
keystrokes into apps that are running elevated (Task Manager,
installers, registry editor), run unIO itself elevated. Otherwise
the OS silently drops the injection attempt.

#### macOS

1. Grant unIO **Accessibility** access: *System Settings → Privacy
   & Security → Accessibility → +* → pick the unIO app.
2. On first launch you'll also be prompted for **Input Monitoring**.
3. **Without these grants** cursor warp and input capture fail
   silently and you'll see log lines like `CGEventTapCreate returned
   NULL` — that's your signal to open System Settings and toggle.

### Tier 2 — Display streaming (JPEG + H.264)

Needed only if you want to route a display from one PC to another
(the Layout canvas's routing feature). Skip this tier and you get
full cursor + keyboard + clipboard sharing; you just can't *see* a
remote PC's pixels on a local panel.

#### All platforms

- **Python `mss`** — already bundled in the binary. Pure user-space
  screen capture, works on Linux X11, Windows, and macOS.

- **`ffmpeg` binary on PATH** — required for the H.264 hardware path
  (phase 5). Without ffmpeg we fall back to the JPEG path, which
  works but is higher latency and larger on the wire.

  ```bash
  # Linux
  sudo apt install ffmpeg
  # Windows (Scoop)
  scoop install ffmpeg
  # macOS (Homebrew)
  brew install ffmpeg
  ```

  **No sudo needed beyond the install itself.**

- **GPU encoder support** — if your GPU supports NVENC / QuickSync /
  VA-API / AMF / VideoToolbox, ffmpeg will pick it up automatically
  at `ffmpeg -encoders`. unIO uses whichever is fastest for H.264.
  Pure CPU fallback (`libx264`) always works.

#### Linux X11 specifics

The cursor-passthrough + window-evictor features need `libX11.so.6`
(comes with X.Org) and a running X server. **Wayland sessions are
out of scope for now** — some capture paths fall through to the
placeholder.

### Tier 3 — Virtual displays (phantom monitors)

Needed only if you want to add an *extra* monitor to a PC that
doesn't physically have one, then show it on another PC's screen
(like extending your single-monitor laptop onto a desktop PC's
second panel). Without this tier the Layout canvas still lets you
*route* your existing displays; you just can't create new ones
out of thin air.

#### Linux — `evdi` kernel module

```bash
# Install the kernel module (DKMS builds it for your kernel):
sudo apt install evdi-dkms libevdi1

# Load it (apt triggers this automatically; verify with):
lsmod | grep evdi

# One-time: create a virtual card so unIO has something to bind to.
# This needs sudo once per boot (or set up a systemd unit / udev
# rule — see below).
sudo sh -c 'echo 1 > /sys/devices/evdi/add'
```

**Why the last sudo command**: `/sys/devices/evdi/add` is mode
`--w-------` root:root. Writing `1` to it tells the kernel to
create a new evdi DRM device. unIO's ctypes bindings then bind to
that device and start streaming its framebuffer. Until you run
this once, virtual displays still appear in the UI but stream a
"no framebuffer backend" placeholder card instead of real pixels.

**Persistent setup** (skip the sudo each boot):

Option A — systemd service:

```ini
# /etc/systemd/system/unio-evdi.service
[Unit]
Description=Create unIO virtual display slot
After=systemd-modules-load.service

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo 1 > /sys/devices/evdi/add'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
```

Then: `sudo systemctl enable --now unio-evdi.service`.

Option B — udev rule that lets a group write `/sys/devices/evdi/add`:

```
# /etc/udev/rules.d/70-unio-evdi.rules
SUBSYSTEM=="evdi", GROUP="input", MODE="0660"
# You also need to widen the add-node permissions — this varies
# by distro; `chgrp input /sys/devices/evdi/add` at boot is the
# simplest path.
```

Option B avoids sudo for unIO entirely but touches system config;
option A is simpler and needs one `sudo systemctl enable` ever.

#### Windows — IDD (Indirect Display Driver)

unIO reuses whatever IDD driver is already installed on the host.
Any of these will work:

- **Microsoft IddSampleDriver** — reference implementation from the
  Windows Driver Samples. Dev-only (unsigned → needs Test Mode).
- **Parsec / Spacedesk / Splashtop** — if you have any of these
  installed, their IDD shows up as a "Virtual Display" monitor that
  unIO can bind to.
- **USBMMIDD_v2** (Amyuni, Apache-licensed) — standalone, ~200 KB.
  Recommended if you don't already have an IDD. Installer signs
  itself as the Amyuni driver so no Test Mode needed.
- **unIO-signed IDD** — not yet shipping; planned for a future
  release with an EV cert so it installs cleanly without the UAC
  driver-signing prompt.

**How unIO picks one up**: at `detect_capabilities()` time we enum
every Windows display via `EnumDisplayDevices` and look for names
containing `"IDD"`, `"Indirect"`, `"Virtual Display"`, or common
vendor strings (`"USBMMIDD"`, `"Spacedesk"`, etc.). The first
match becomes the framebuffer source when you hit + Virtual in
the Layout canvas.

**Sudo / admin needed**: only once, to install the driver. After
that, running unIO as a normal user just binds to the already-
present virtual monitor.

### Running from source

Python 3.10+ and:

```bash
pip install -r requirements.txt
```

requirements.txt pulls:
- `pyyaml` (config persistence)
- `Pillow` (image encode/decode)
- `psutil` (interface enumeration for mesh discovery)
- `mss` (cross-platform screen capture)

ffmpeg, evdi, and IDD drivers are **NOT** installed by
`requirements.txt` — they're system-level and platform-specific
(see Tiers 2 and 3 above).

### Quick cheat-sheet

| Want to… | Linux | Windows | macOS |
|---|---|---|---|
| Share cursor + keyboard + clipboard | `sudo usermod -aG input $USER` (once) | nothing | grant Accessibility |
| Show a remote display on a local panel | `sudo apt install ffmpeg` | `scoop install ffmpeg` | `brew install ffmpeg` |
| Hardware H.264 encode (<20 ms) | `ffmpeg` with NVENC/VA-API/QSV | `ffmpeg` with NVENC/QSV/AMF | `ffmpeg` with VideoToolbox |
| Add a virtual display to this PC | `sudo apt install evdi-dkms` + one-time `sudo sh -c 'echo 1 > /sys/devices/evdi/add'` | install any IDD driver (Parsec / USBMMIDD / Spacedesk) | n/a — out of scope |

## Quick Start

### Install

Grab a pre-built binary for your OS from the
[Releases page](https://github.com/4d1-1010/unIO/releases):

```bash
# Linux — raw binary or AppImage (the AppImage ships the unIO icon
# + .desktop entry, which is what file managers / docks will render)
tar xzf unio-*-linux-x64.tar.gz && ./unio
# or
chmod +x unIO-*-x86_64.AppImage && ./unIO-*-x86_64.AppImage

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
> forward — unIO logs `Keyboard capture disabled` when it hits this.

1. Launch unIO on every computer you want to share input across. No
   host / join choice — each PC runs a peer that immediately starts
   announcing itself on every local interface.
2. Watch the Activity tab. Other unIO peers appear as they announce
   themselves. The mesh auto-connects within a couple of seconds (one
   side dials, tiebreak by machine id). A manual **Connect** pill on
   each discovered-peer row forces a dial if you're impatient.
3. Open the **Layout** tab on any connected machine. Drag each
   machine's monitors until they match the physical arrangement,
   then click **Apply layout** — the new positions gossip via LWW
   to every peer and unIO reconfigures each PC's OS display
   positions so the cursor crosses at the seams you defined.

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

If you want to hack on unIO or skip the binary download:

```bash
pip install -r requirements.txt
python scripts/launcher.py
```

The mesh architecture removed the server/client split, so the old
`run_server.py` / `run_client.py` / `run_configurator.py` helper
scripts are gone — there's nothing to drive headlessly that the
shell doesn't already cover by just running. File transfers still
have their own CLI at `scripts/send_file.py`.

### Building a standalone binary

See `packaging/README.md`. Short version:

```bash
pip install pyinstaller
python packaging/build.py --clean
# → Linux:   dist/unio  +  dist/unIO-x86_64.AppImage (if appimagetool on PATH)
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

Files are saved to `~/unIO-Received/` on the target machine.

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
| MOUSE_MOVE_REL | 0x11 | peer → active | Relative mouse delta (direct, no relay) |
| MOUSE_BUTTON | 0x12 | peer → active | Mouse button press/release |
| MOUSE_SCROLL | 0x13 | peer → active | Scroll wheel |
| KEY_EVENT | 0x14 | peer → active | Keyboard press/release |
| CLIPBOARD_UPDATE | 0x30 | peer → peers | Clipboard content gossip |
| IDENTIFY | 0x50 | peer → peer | Show numbered overlay on target peer's monitors |
| HEARTBEAT | 0xF0 | peer ↔ peer | Keep-alive ping |
| HEARTBEAT_ACK | 0xF1 | peer ↔ peer | Keep-alive reply |
| HELLO | 0x60 | peer ↔ peer | Identity + presence at link setup |
| STATE_SYNC | 0x61 | peer → newcomer | Full LWW store dump |
| SET_STATE | 0x62 | peer → peers | LWW register update (gossip) |
| CURSOR_RELEASE | 0x63 | peer → peer | Direct cursor ownership handoff |

Legacy message types (`REGISTER`, `REGISTER_ACK`, `LAYOUT_UPDATE`,
`LAYOUT_APPLY`, `APPLY_MONITORS`, `EDGE_HIT`, `ACTIVATE`, `DEACTIVATE`,
`SET_INPUT_*`, etc.) are still defined in `protocol.py` for wire
compatibility with older clients, but the mesh replaces all of them
with the peer-to-peer messages above.
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

1. **Active peer** polls cursor position at 120 Hz.
2. When the cursor reaches a screen edge, the peer checks the
   replicated layout for an adjacent monitor on another peer.
3. If found, the active peer sends `CURSOR_RELEASE` directly to
   the target peer (no relay — there's no server).
4. The target warps the cursor to the entry point, flips itself to
   ACTIVE mode, and gossips `SET_STATE(active=self)` so the rest of
   the mesh agrees on who owns the cursor.
5. The source peer flips to FORWARDING, grabs pointer + keyboard,
   and forwards events directly to the new active peer.
6. When the cursor later hits an edge back, the process reverses.

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
