# xorio-app

Desktop runtime + licensing tooling for unIO. C++20, Dear ImGui,
no GLFW / no SDL.

## Layout

| Path | Scope |
|---|---|
| `xorio/` | Runtime binary (`xorio-ui`). Win32 + D3D11 on Windows; Xlib + EGL + desktop GL on Linux. Verifies pasted licence tokens at runtime. |
| `licensing/` | Vendor key material (`dev/`) + the `xorio-license-tool` CLI that signs tokens. Dev-side only — not shipped to end users. |
| `cmake/` | Shared CMake helpers: `EmbedBinary` (binary blob → header) and `UnioCrypto` (libcrypto interface target). |
| `packaging/` | Docker-driven builds for Linux + Windows. |
| `dist/` | Build outputs: `linux-x64/` and `win-x64/`. Created by the build scripts. |

The two subdirs share the licence wire-format via the
`xorio::license_format` static library so the bytes the tool signs
are byte-for-byte the bytes the runtime verifies.

## Build

```bash
# All platforms in one go
./packaging/build-all.sh

# Single platform
./packaging/docker/build-linux.sh
./packaging/docker/build-win.sh
```

Outputs land in `dist/linux-x64/` and `dist/win-x64/`. The
binaries are routed there directly by CMake — no extract step.

For a whole-repo build (also produces `xorio-pipe`), use the
repo-root `packaging/docker/build-linux.sh` instead.

## Issuing a dev licence

```bash
./dist/linux-x64/xorio-license-tool \
    --key licensing/dev/vendor_master.key \
    --customer user@example.com \
    --tier pro \
    --duration 5d
```

Paste the printed token into the runtime app's Access tab. See
`licensing/dev/README.md` for full details and the rotation
procedure before launch.
