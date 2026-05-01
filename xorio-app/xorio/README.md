# xorio

Native C++ UI layer for xorIO. Replaces the Python / Tkinter
`xorio/apps/` tree (kept in the repo until the port is
feature-complete, then deleted in a separate PR).

## Stack

| Layer | Choice | Notes |
|---|---|---|
| UI toolkit | [Dear ImGui](https://github.com/ocornut/imgui) | MIT, vendored pinned tag; see `cmake/imgui.cmake` |
| Platform (windowing + input) | raw **Win32** (Windows) + raw **Xlib** (Linux) + **Cocoa** (macOS, future) | No GLFW / SDL. `imgui_impl_win32` is upstream; `imgui_impl_x11` is in-house under `src/platform/x11/`. |
| Rendering | **D3D11** (Windows) + **OpenGL 3.3 via EGL** (Linux) + **Metal** (macOS, future) | Native per OS — enables zero-copy video preview from `xorio-pipe`'s presenter texture. |
| C++ standard | C++20 | Matches `xorio-pipe/`. |

Locked in [ARCHITECTURE.md §5](../ARCHITECTURE.md) decisions #1 / #1a / #1b.

## Building

### Linux (native)

```bash
cmake -S xorio -B xorio/build -DCMAKE_BUILD_TYPE=Release
cmake --build xorio/build -j
./xorio/build/xorio
```

Requires X11 (`libX11`, `libxcb`), EGL (`libEGL`, `libGLESv2`
headers are not used — we link desktop GL via `libGL`), and a
C++20 compiler (Clang 18 / GCC 13).

### Windows (msvc-wine in Docker)

TODO (follow-up PR — mirrors `packaging/docker/build-win.sh` for
`xorio-pipe`).

## Status

Scaffold only. Does nothing beyond opening a blank window and
clearing to the paper background colour. Subsequent PRs port:

1. `ui_theme.py` → `src/theme.{hpp,cpp}` — palette + primitives
2. `shell.py` → `src/screens/shell.{hpp,cpp}` — main window
3. `layout_panel.py` → `src/screens/layout.{hpp,cpp}` — drag/drop canvas
4. `peer.py` → `src/screens/peers.{hpp,cpp}` — peer list + host/join
5. `stream_window.py` + `source_overlay.py` + `log_view.py`

Until step 5 is done, the Python tree at `xorio/` is still the
runnable app (`python -m xorio.apps.shell`).
