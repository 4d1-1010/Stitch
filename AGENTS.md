# unIO Agent Guide

## Project Overview

unIO is a cross-machine distributed input routing system that unifies multiple computers into a single virtual workspace. It enables seamless cursor/keyboard/clipboard sharing across machines on a LAN using a full-mesh P2P architecture with Last-Writer-Wins (LWW) eventual consistency.

## Commands

### Development
```bash
# Run tests
python -m pytest tests/
python -m unio.apps.shell    # Launch UI for manual testing

# Build native helper (C++ control plane)
cd unio-pipe && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

# Build Python distribution
python packaging/build.py                 # end-user build
python packaging/build.py --clean         # wipe dist/build first
python packaging/build.py --dev-logs      # enable diagnostic UI (bakes DEV_LOGS=True)
```

### Build Output
- **Linux**: `dist/unio` (ELF), `dist/unIO-x86_64.AppImage`
- **Windows**: `dist/unio.exe`
- **macOS**: `dist/unio` (Mach-O), `dist/unio.app` bundle

## Architecture

### Core Components

```
unio/                    # Main Python application
├── apps/                # UI components
│   ├── shell.py         # Main window, session lifecycle
│   ├── layout_panel.py  # Display arrangement canvas
│   ├── peer.py          # P2P mesh peer
│   ├── stream_window.py # Remote display overlay
│   └── log_view.py      # Dev log buffer
├── core/                # Protocol & state
│   ├── protocol.py      # Wire format, message types
│   ├── layout.py        # Global coordinate manager
│   ├── discovery.py     # LAN mesh discovery (UDP)
│   └── network.py       # TCP connections
├── features/            # Platform-specific implementations
│   ├── capture_xcomposite.py  # X11 XComposite screen capture
│   ├── capture_pipewire.py    # Wayland PipeWire + Portal capture
│   ├── capture_backend.py     # Abstract CaptureBackend ABC
│   ├── display_server.py      # X11 vs Wayland detection
│   ├── display_stream.py      # Stream sink (TCP reader)
│   └── hw_pipeline.py         # VA-API encoder (Linux)
└── backends/            # OS abstraction layers
    ├── linux_x11.py
    ├── windows.py
    └── macos.py

unio-pipe/               # C++ native helper (PR 6)
├── src/
│   ├── control_socket.cpp      # UDS/named pipe control plane
│   ├── encoder_vaapi.cpp       # VA-API H.264 encoder
│   ├── capture_xcomposite.cpp  # X11 capture backend
│   ├── capture_pipewire.cpp    # Wayland PipeWire capture backend
│   └── stream_manager.cpp      # Stream lifecycle
├── include/
│   ├── capture_xcomposite.h
│   └── capture_pipewire.h
```

### Control Flow

1. **Startup**: `shell.py:MainWindow` creates UI, starts `_AsyncRunner` thread with asyncio loop, launches `Peer` (P2P mesh).

2. **Discovery**: `MeshDiscovery` broadcasts UDP announces on LAN. Signed-in peers (admin/admin test auth) activate the mesh.

3. **P2P Mesh**: Each peer maintains TCP connections to all others. On connect:
   - Send `HELLO` → learn peer identity + monitors
   - Sender of `HELLO` sends `STATE_SYNC` → full LWW store dump
   - Subsequent changes gossiped via `SET_STATE`

4. **Layout Management**: `LayoutManager` maintains global virtual coordinate space. Handles edge detection (8px ENTRY_INSET prevents bounce) and cursor transitions between monitors.

5. **Display Streaming** (Phase 0): Routes map `sink_key` → `source_key`. `StreamSink` connects to source peer, `StreamWindow` renders JPEG frames on borderless overlay.

### Wire Protocol (protocol.py)

Frame format: `[type: uint16][length: uint32][payload: JSON]`

Key message types:
- **Lifecycle**: `REGISTER`, `REGISTER_ACK`, `LAYOUT_UPDATE`
- **Input**: `MOUSE_MOVE_ABS`, `MOUSE_MOVE_REL`, `MOUSE_BUTTON`, `KEY_EVENT`
- **Handoff**: `EDGE_HIT`, `HANDOFF`, `ACTIVATE`, `DEACTIVATE`
- **P2P**: `HELLO`, `STATE_SYNC`, `SET_STATE`, `CURSOR_RELEASE`, `SWAP_INPUT`
- **Clipboard**: `CLIPBOARD_UPDATE`
- **File Transfer**: `FILE_OFFER`, `FILE_ACCEPT`, `FILE_CHUNK`, `FILE_DONE`

### Capture Architecture

```
display_stream.py:_capture_backend()
  → display_server.detect() → "x11" | "wayland" | None
  → XCompositeCapture (X11) | PipeWireCapture (Wayland) | WGCCapture (Windows)
```

Each backend implements the `CaptureBackend` ABC in `capture_backend.py`:
- `open()` → bool (probe + initialize)
- `close()` → release resources
- `grab(rect: dict)` → PIL.Image RGB or None
- `set_exclude_xids(xids)` → overlay exclusion
- `last_bgra_bytes()` → raw BGRA buffer (H.264 fast path)

Linux auto-selects the backend based on the display server:
- `XDG_SESSION_TYPE=x11` or `$DISPLAY` set → XComposite
- `XDG_SESSION_TYPE=wayland` or `$WAYLAND_DISPLAY` set → PipeWire
- Both unset → probe both, prefer the working one

The C++ native helper (`unio-pipe`) has parallel capture backends:
- `capture_xcomposite.cpp` for X11 (same as Python)
- `capture_pipewire.cpp` for Wayland (libpipewire + libdbus-1 for portal)

## Code Patterns

### Data Classes
All messages use `@dataclass` with `asdict()` for serialization:
```python
@dataclass
class MouseMoveRelMsg:
    dx: int
    dy: int
```

### LWW Store
Peer maintains replicated state via `LWWStore`. Keys:
- `active:<machine_id>` → who owns cursor
- `workspace:<id>` → workspace config (members, lock, settings)
- `route:<ws_id>` → display stream routing table
- `hubs:<ws_id>` → hub registry (virtual display infrastructure)

### Platform Abstraction
Backends implement common interfaces:
- `CaptureBackend`: screen capture (mss, XComposite, DXGI)
- `EncoderBackend`: video encoding (VA-API, software fallback)
- `StreamServer`: TCP stream provider

## Testing

```bash
# Unit tests (protocol, layout)
python tests/test_core.py

# Run full test suite
pytest tests/ -v
```

Test coverage focuses on:
- Protocol roundtrip (encode/decode)
- Layout coordinate conversion
- Edge crossing detection
- Multi-monitor scenarios

## Gotchas & Conventions

### Asyncio Thread Model
UI runs on Tk main thread. All async I/O (`Peer`, `StreamSink`) runs on dedicated `_AsyncRunner` thread. Use `asyncio.run_coroutine_threadsafe()` to submit coroutines from UI thread.

### Dev Logs Flag
`DEV_LOGS = bool(os.environ.get("UNIO_DEV_LOGS"))` in `__init__.py`. Flip to `True` via:
- `UNIO_DEV_LOGS=1` environment variable
- `python packaging/build.py --dev-logs` (bakes into binary)

### Coordinate Systems
- **Global**: unified virtual space across all machines
- **Local**: OS screen coordinates (X11, Windows Display Settings)
- Conversion: `LayoutManager.local_to_global()` / `global_to_local()`

### Edge Thresholds
- `EDGE_THRESHOLD = 1` (client-side detection)
- `ENTRY_INSET = 8` (prevent immediate bounce on activation)
- `SEAM_TOLERANCE = 32` (adjacency matching with gap tolerance)

### Workspace Model
- Workspaces group PCs for routing
- Lock model: any PC can lock; only locker can unlock/edit
- Routes are workspace-scoped: `route:<ws_id>` in LWW
- Default: identity routing (each monitor shows its own PC)

### Stream Window Lifecycle
1. Route change triggers `_sync_display_streams()`
2. For each sink monitor: resolve effective source via `_resolve_route_chain()`
3. If source ≠ sink: create `StreamWindow` + `StreamSink`
4. If source = sink or disappeared: teardown

### Window Evictor (Linux only)
X11 evictor pushes apps off reserved panels when source overlays are live. Prevents apps from landing on "dark" source panels during swap routing.

### Build Dependencies
- **Linux**: `libx11-dev`, `libxtst-dev`, `libxinerama-dev`, `libva-dev`, `libpipewire-0.3-dev`, `libspa-0.2-dev`, `libdbus-1-dev`, `libfuse2`
- **macOS**: PyInstaller with `--osx-bundle-identifier`
- **Windows**: PyInstaller onefile mode

## Naming Conventions

- `*_msg.py`: message dataclasses (protocol.py)
- `*_backend.py`: platform implementations (backends/)
- `*_capture.py`, `*_stream.py`: feature modules (features/)
- `stream_window`: overlay for remote display
- `source_overlay`: overlay covering "projected away" panels (legacy)

## File Structure Notes

- `assets/`: PNG icons for Tk widgets (48px logo, 28px tab icons)
- `packaging/`: build scripts, PyInstaller spec, AppImage packaging
- `tests/`: core protocol and layout tests (no UI tests yet)
- `scripts/`: utility scripts (launcher, send_file, logo generation)

## Common Tasks

### Add a new message type
1. Add to `MsgType` enum in `protocol.py`
2. Create `@dataclass` for payload
3. Register in `_MSG_CLASS` mapping
4. Update encode/decode logic if needed

### Add a new feature
1. Create module in `features/` with platform-specific backends in `backends/`
2. Wire into `shell.py` lifecycle (e.g., `_sync_*` method)
3. Add LWW key if stateful (e.g., `route:<ws_id>`)

### Modify wire protocol
1. Update `protocol.py`
2. Run `tests/test_core.py` to verify roundtrip
3. Consider backward compatibility (deprecated messages kept for old clients)

### Build with native helper
1. Ensure `cmake` and `libva-dev` installed (Linux)
2. `python packaging/build.py` auto-builds `unio-pipe` if cmake available
3. Helper binary lands in `dist/unio-pipe` for PyInstaller bundling