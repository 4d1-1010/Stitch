# Vendored Dear ImGui

Pinned at **`v1.92.7`** (commit in `COMMIT`).

| File | Licence |
|---|---|
| `LICENSE.txt` | MIT — © 2014-2025 Omar Cornut |

SPDX: `MIT`. https://github.com/ocornut/imgui/blob/master/LICENSE.txt

## What's vendored

| Path | Purpose |
|---|---|
| `imgui.{h,cpp}` | Core |
| `imgui_widgets.cpp` | Widget impls |
| `imgui_draw.cpp` | Draw-list compiler |
| `imgui_tables.cpp` | Tables API |
| `imgui_demo.cpp` | `ImGui::ShowDemoWindow()` — used as a smoke test |
| `imgui_internal.h` | Private API we link against |
| `imconfig.h` | Compile-time config (unmodified) |
| `imstb_{rectpack,textedit,truetype}.h` | Bundled stb headers |
| `misc/cpp/imgui_stdlib.{h,cpp}` | `std::string` helpers |
| `backends/imgui_impl_win32.{h,cpp}` | Upstream Win32 platform backend |
| `backends/imgui_impl_dx11.{h,cpp}` | Upstream D3D11 renderer |
| `backends/imgui_impl_opengl3.{h,cpp}` + `imgui_impl_opengl3_loader.h` | Upstream GL3 renderer (Linux; Mac until Metal lands) |

## What's NOT vendored

- `backends/imgui_impl_glfw.*`, `imgui_impl_sdl*.*` — we don't use
  GLFW or SDL (ARCHITECTURE.md §5 decision #1a: raw Win32 + Xlib).
- `backends/imgui_impl_x11.*` — **does not exist upstream.** We
  wrote `unio-ui/src/platform/x11/imgui_impl_x11.{hpp,cpp}`
  ourselves; see that file.
- `examples/` + `docs/` — not needed for the library build.
- `backends/imgui_impl_metal.*` — will vendor when macOS lands.

## Updating

```bash
cd /tmp
git clone --depth 1 --branch vX.Y.Z https://github.com/ocornut/imgui.git imgui-src
# Copy the files listed above; update VERSION + COMMIT; test.
```

Keep the file list above in sync with what lives here. The
CMakeLists.txt in this directory lists every source file explicitly;
new files won't compile in silently.
