# Vendored Inter

Pinned at **`v3.19`** (static TTF release, "Hinted for Windows"
variants — ship cleanly on stb_truetype via ImGui).

| File | Size | Use |
|---|---|---|
| `Inter-Regular.ttf` | 680 KB | Body text, labels, status lines |
| `Inter-Bold.ttf`    | 715 KB | Buttons, headings, emphasis |

SPDX: `OFL-1.1`. https://github.com/rsms/inter/blob/master/LICENSE.txt

## Why Inter

- **Commercial-safe.** SIL Open Font License 1.1 explicitly permits
  embedding in closed-source software, modification, and
  redistribution (matching `feedback_commercial_license.md`).
- **Product-UI-native.** Used by Figma, GitHub, Vercel, Linear,
  Notion — reads as neutral at small sizes and holds up at title
  size.
- **Static TTFs available.** v4.x went variable-only; v3.19's
  static TTFs are what stb_truetype handles cleanly.

## Why not bundle italic (yet)

Inter-Italic.ttf adds another ~700 KB. Only one place in the UI
currently wants italic (the alone-state status line), and we
render it as muted-regular there instead. If italic becomes
widespread we vendor it — one-line change.

## Loading

See `unio-ui/src/theme.cpp` — `theme::load_fonts()` registers the
faces with `io.Fonts->AddFontFromFileTTF` at each size token
(`font::size_xs`..`font::size_title`). Each (face, size) pair
becomes an `ImFont*` accessed via the `theme::font::*` symbols.

## Updating

```bash
curl -L -o /tmp/inter.zip \
  "https://github.com/rsms/inter/releases/download/vX.Y/Inter-X.Y.zip"
unzip -jq /tmp/inter.zip \
  "Inter Hinted for Windows/Desktop/Inter-Regular.ttf" \
  "Inter Hinted for Windows/Desktop/Inter-Bold.ttf" \
  -d unio-ui/vendor/inter/
# Update VERSION, verify LICENSE.txt unchanged.
```
