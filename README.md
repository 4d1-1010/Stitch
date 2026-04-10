# Unified Desktop

A distributed input routing system that makes multiple Linux PCs behave like a single multi-monitor desktop. Move your mouse seamlessly across machines, type on whichever screen has focus, and share your clipboard — all over the network.

**This is NOT a remote desktop.** Each machine renders its own display. Unified Desktop only routes keyboard/mouse input and synchronizes clipboard content.

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
| Protocol | `ud/protocol.py` | Wire format, message types, serialization |
| Layout Manager | `ud/layout.py` | Global coordinate space, edge detection, handoff computation |
| X11 Bindings | `ud/x11.py` | Cursor control, input injection (XTest), monitor detection (Xinerama) |
| Keyboard Capture | `ud/keyboard.py` | Keystroke capture via Linux evdev |
| Clipboard Sync | `ud/clipboard.py` | Clipboard monitoring and sync via xclip/xsel |
| File Transfer | `ud/filetransfer.py` | Send files between machines |
| Network Layer | `ud/network.py` | Async TCP with framed messages |
| Server | `ud/server.py` | Central coordinator: layout, routing, handoff |
| Client | `ud/client.py` | Per-machine agent: capture, inject, edge detect |

## Requirements

- **OS:** Linux with X11 (Wayland not supported in MVP)
- **Libraries:** libX11, libXtst, libXinerama (standard on most distros)
- **Python:** 3.10+
- **Clipboard tools:** `xclip` or `xsel` (for clipboard sync)
- **Keyboard forwarding:** User must be in the `input` group (or run as root)

### Install dependencies

```bash
# Debian/Ubuntu
sudo apt install libx11-dev libxtst-dev libxinerama-dev xclip python3-yaml

# Fedora
sudo dnf install libX11-devel libXtst-devel libXinerama-devel xclip python3-pyyaml

# Arch
sudo pacman -S libx11 libxtst libxinerama xclip python-yaml

# For keyboard forwarding (add user to input group)
sudo usermod -aG input $USER
# Log out and back in for group change to take effect

# Python deps
pip install pyyaml
```

## Quick Start (Two Machines)

### 1. Configure the layout

Edit `config.yaml` on the server machine:

```yaml
layout:
  machines:
    pc1:
      offset_x: 0
      offset_y: 0
    pc2:
      offset_x: 1920    # starts right after PC1's 1920px monitor
      offset_y: 0
```

If you skip this, monitors are auto-placed left-to-right in connection order.

### 2. Start the server

On any machine (can be one of the PCs, or a third machine):

```bash
python run_server.py --port 24800
# Add -v for verbose/debug output
```

### 3. Start clients

On PC1 (the left machine):
```bash
python run_client.py --id pc1 --server SERVER_IP --port 24800
```

On PC2 (the right machine):
```bash
python run_client.py --id pc2 --server SERVER_IP --port 24800
```

### 4. Use it

- Move your mouse to the **right edge** of PC1's screen — the cursor appears on PC2
- Move to the **left edge** of PC2 — the cursor returns to PC1
- Keyboard input follows the cursor automatically
- Copy text on one machine, paste on the other (clipboard sync)

## File Transfer

Send a file from one machine to another:

```bash
python send_file.py --server SERVER_IP --from pc1 --to pc2 --file /path/to/file.pdf
```

Files are saved to `~/UnifiedDesktop-Received/` on the target machine.

## Configuration Reference

```yaml
server:
  host: "0.0.0.0"      # bind address
  port: 24800           # TCP port

layout:
  machines:
    machine_id:          # must match --id passed to run_client.py
      offset_x: 0       # global X origin for this machine's monitors
      offset_y: 0       # global Y origin
```

### Multi-monitor machines

If a machine has multiple monitors, they're detected automatically via Xinerama. The `offset_x`/`offset_y` shifts the entire monitor group. Local monitor positions (from Xinerama) are preserved relative to each other.

Example: PC1 has two 1920x1080 monitors side-by-side, PC2 has one:
```yaml
layout:
  machines:
    pc1:
      offset_x: 0       # PC1's monitors at (0,0) and (1920,0)
      offset_y: 0
    pc2:
      offset_x: 3840    # after both of PC1's monitors
      offset_y: 0
```

### Vertical layouts

```yaml
layout:
  machines:
    top_pc:
      offset_x: 0
      offset_y: 0
    bottom_pc:
      offset_x: 0
      offset_y: 1080    # below the top PC's 1080px tall monitor
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
| MOUSE_MOVE_REL | 0x11 | C→S→C | Relative mouse delta |
| MOUSE_BUTTON | 0x12 | C→S→C | Mouse button press/release |
| MOUSE_SCROLL | 0x13 | C→S→C | Scroll wheel |
| KEY_EVENT | 0x14 | C→S→C | Keyboard press/release |
| EDGE_HIT | 0x20 | C→S | Cursor reached screen edge |
| ACTIVATE | 0x22 | S→C | You now own the cursor |
| DEACTIVATE | 0x23 | S→C | Release cursor, start forwarding |
| CLIPBOARD_UPDATE | 0x30 | C→S→C | Clipboard content changed |
| FILE_OFFER | 0x40 | C→S→C | File transfer initiation |
| FILE_CHUNK | 0x42 | C→S→C | File data chunk |
| FILE_DONE | 0x43 | C→S→C | File transfer complete |
| HEARTBEAT | 0xF0 | S→C | Keep-alive ping |

## How Cursor Handoff Works

1. **Active machine** polls cursor position at 120Hz
2. When cursor reaches a screen edge, check the global layout for an adjacent monitor on a different machine
3. If found, send `EDGE_HIT` to the server
4. Server computes the entry point on the target monitor
5. Server sends `DEACTIVATE` to the source, `ACTIVATE` to the target
6. Source grabs pointer+keyboard and starts forwarding input as relative deltas
7. Target warps cursor to entry point and injects received events via XTest
8. When the cursor later hits an edge back, the process reverses

## Troubleshooting

**"Cannot open X display"**
- Ensure `$DISPLAY` is set (usually `:0` or `:1`)
- Run from within an X11 session (not pure Wayland)

**"No keyboard input devices found"**
- Add your user to the `input` group: `sudo usermod -aG input $USER`
- Log out and back in

**Cursor doesn't cross to the other machine**
- Check that `config.yaml` offsets align monitor edges (e.g., PC2's offset_x = PC1's total width)
- Ensure both clients registered (check server log output)
- Use `-v` flag for debug logging

**Clipboard not syncing**
- Install `xclip`: `sudo apt install xclip`
- Verify it works: `echo test | xclip -selection clipboard && xclip -selection clipboard -o`

**High latency**
- Ensure machines are on the same LAN
- Check network with `ping`
- The server should ideally run on the same machine as one of the clients

## Security Notes

- Traffic is **unencrypted** in the MVP. Input events (including keystrokes) are sent in plaintext.
- For production use, tunnel through SSH: `ssh -L 24800:localhost:24800 server-host`
- Only run on trusted networks.

## Limitations (MVP)

- Linux + X11 only (no Wayland, Windows, or macOS)
- No encryption (use SSH tunneling)
- Keyboard forwarding requires `input` group membership
- No drag-and-drop file transfer (CLI only)
- No per-application hotkey passthrough
