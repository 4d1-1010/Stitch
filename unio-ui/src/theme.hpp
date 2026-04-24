/*! @file theme.hpp
 *  @brief Paper-lilac design tokens + ImGui primitives.
 *
 *  Port of `unio/apps/ui_theme.py`'s paper-lilac surface (the
 *  target look; the Python file's *legacy dark* palette is
 *  deliberately not ported — that UI is being killed).
 *
 *  Usage:
 *    1. Call @ref unio_ui::theme::apply_style once after
 *       `ImGui::CreateContext()` and before any `NewFrame()`.
 *    2. Consume tokens (`palette::lilac`, `space::md`, etc.) and
 *       primitives (`pill_button`, `status_dot`, ...) from
 *       screens.
 */
#pragma once

#include "imgui.h"

namespace unio_ui::theme {

/// @brief RGB-hex helper so the palette reads like CSS.
/// @param rgb 0xRRGGBB packed value.
/// @param a   Alpha in 0..1 (default 1.0).
constexpr ImVec4 rgba_hex(unsigned int rgb, float a = 1.0f) {
    return ImVec4(
        ((rgb >> 16) & 0xFFu) / 255.0f,
        ((rgb >>  8) & 0xFFu) / 255.0f,
        ((rgb      ) & 0xFFu) / 255.0f,
        a);
}

// ── Palette ───────────────────────────────────────────────────
// Kept minimal on purpose: one accent (lilac), one positive
// (mint), one warn (amber), one danger (coral). Everything else
// is neutral so status dots + the single accent carry meaning.
namespace palette {

inline constexpr ImVec4 paper_bg        = rgba_hex(0xffffff);  ///< App background.
inline constexpr ImVec4 paper_surface   = rgba_hex(0xf4f5f8);  ///< Cards, content panels.
inline constexpr ImVec4 paper_rail      = rgba_hex(0xeef0f5);  ///< Left tab rail.
inline constexpr ImVec4 paper_rail_deep = rgba_hex(0xc9cfd9);  ///< Outer mini rail / identity bar.
inline constexpr ImVec4 paper_border    = rgba_hex(0xe2e4ec);  ///< Hairline separators.
inline constexpr ImVec4 paper_text      = rgba_hex(0x1f2024);  ///< Primary text.
inline constexpr ImVec4 paper_muted     = rgba_hex(0x6d7286);  ///< Secondary text.
inline constexpr ImVec4 paper_faint     = rgba_hex(0x9a9db0);  ///< Tertiary / hint text.

inline constexpr ImVec4 lilac       = rgba_hex(0x8b7bff);  ///< Primary accent.
inline constexpr ImVec4 lilac_hover = rgba_hex(0x7a6af0);  ///< Hover shade of @ref lilac.
inline constexpr ImVec4 lilac_soft  = rgba_hex(0xeeeaff);  ///< Tint behind active tab.
inline constexpr ImVec4 mint        = rgba_hex(0x5cc9a3);  ///< Success / connected.
inline constexpr ImVec4 amber       = rgba_hex(0xe8b04c);  ///< Warn / high latency.
inline constexpr ImVec4 coral       = rgba_hex(0xff6b5b);  ///< Danger / disconnected / error.

}  // namespace palette

// ── Typography (points) ───────────────────────────────────────
namespace font {

inline constexpr float size_xs    =  9.0f;
inline constexpr float size_sm    = 10.0f;
inline constexpr float size_base  = 11.0f;
inline constexpr float size_lg    = 13.0f;
inline constexpr float size_xl    = 16.0f;
inline constexpr float size_title = 22.0f;

/*! @brief Handles into the ImGui font atlas, populated by
 *  @ref load_fonts. `body` is the default. Nullptr before
 *  load_fonts() runs.
 */
extern ImFont* body;         ///< Inter Regular @ size_base.
extern ImFont* body_sm;      ///< Inter Regular @ size_sm.
extern ImFont* body_xs;      ///< Inter Regular @ size_xs.
extern ImFont* body_lg;      ///< Inter Regular @ size_lg.
extern ImFont* bold;         ///< Inter Bold @ size_base.
extern ImFont* bold_xs;      ///< Inter Bold @ size_xs (rail labels).
extern ImFont* bold_xl;      ///< Inter Bold @ size_xl (rail glyphs).
extern ImFont* title;        ///< Inter Bold @ size_title.

}  // namespace font

// ── Spacing + radii (px) ──────────────────────────────────────
namespace space {
inline constexpr float xs =  4.0f;
inline constexpr float sm =  8.0f;
inline constexpr float md = 12.0f;
inline constexpr float lg = 18.0f;
inline constexpr float xl = 28.0f;
}  // namespace space

namespace radius {
inline constexpr float sm =  6.0f;
inline constexpr float md = 12.0f;
inline constexpr float lg = 18.0f;
}  // namespace radius

/*! @brief Build the font atlas from the embedded Inter TTFs.
 *
 *  Call once after `ImGui::CreateContext()` and *before* the
 *  platform renderer init (`ImGui_ImplDX11_Init` / `_OpenGL3_Init`)
 *  so the renderer sees the final atlas. Populates @ref font::body
 *  and friends.
 */
void load_fonts();

/*! @brief Apply the paper-lilac palette to ImGui's active style.
 *
 *  Call once after @ref load_fonts, before the first
 *  `NewFrame()`. Sets all @c ImGuiCol_ entries + rounding +
 *  spacing to the tokens above.
 */
void apply_style();

// ── Primitives ────────────────────────────────────────────────

/// Semantic variants of @ref pill_button.
enum class PillVariant {
    Primary,    ///< Lilac fill, white text.
    Secondary,  ///< Paper surface fill, dark text.
    Ghost,      ///< No fill, dark text, surface on hover.
    Danger,     ///< Coral fill, white text.
};

/*! @brief Flat rounded button with semantic variants.
 *  @param label UTF-8 text.
 *  @param variant See @ref PillVariant.
 *  @param size_override Font size in pt; 0 = use @ref font::size_base.
 *  @return `true` on click (same semantics as @c ImGui::Button).
 */
bool pill_button(const char* label,
                 PillVariant variant = PillVariant::Primary,
                 float size_override = 0.0f);

/// Semantic states for @ref status_dot.
enum class DotState {
    Ok,    ///< Connected / healthy (mint).
    Warn,  ///< High latency / degraded (amber).
    Bad,   ///< Disconnected / error (coral).
    Idle,  ///< Unknown / no peer (faint grey).
};

/*! @brief Small filled circle for connection state.
 *  @param state See @ref DotState.
 *  @param size Diameter in px (default 10 matches the Tk impl).
 */
void status_dot(DotState state, float size = 10.0f);

/*! @brief 1-px separator line.
 *  @param axis `'x'` for a horizontal rule, `'y'` for a vertical one.
 */
void hairline(char axis = 'x');

/// Vector-drawn icons used by @ref rail_button.
///
/// We draw the icons with @c ImDrawList rather than pull them
/// from the font atlas: Inter covers Latin fully but does not
/// include the Miscellaneous Technical / Dingbats glyphs we
/// originally used (⏵ ▦ ⚙ ⊕), which rendered as empty rects.
/// Hand-drawn vectors are crisp at any DPI and colour-themable.
enum class RailIcon {
    Activity,  ///< Right-pointing triangle.
    Layout,    ///< 2×2 grid of squares.
    Settings,  ///< Gear / cog.
    Access,    ///< Padlock.
    Help,      ///< Circled "?".
};

/*! @brief Left-rail navigation entry: icon + label stacked.
 *
 *  Active entries get a lilac-soft background + lilac icon / label
 *  tint.
 *
 *  @param id     ImGui ID for state (unique per rail).
 *  @param icon   Which vector glyph to draw.
 *  @param label  Caption text below the icon.
 *  @param active `true` if this is the selected entry.
 *  @return `true` on click.
 */
bool rail_button(const char* id, RailIcon icon,
                 const char* label, bool active);

/*! @brief Begin a paper-surface card with optional accent strip.
 *
 *  Creates a child window with the card bg + paddings. Pair with
 *  @ref end_card.
 *
 *  @param id     Unique ImGui ID.
 *  @param size   Card size (0,0 = auto-fit to content).
 *  @param accent Optional left-border colour (3 px strip). @c nullptr = no strip.
 *  @param pad    Interior padding (default @ref space::lg).
 */
void begin_card(const char* id, ImVec2 size = ImVec2(0, 0),
                const ImVec4* accent = nullptr,
                float pad = space::lg);

/// Close the card opened by @ref begin_card.
void end_card();

}  // namespace unio_ui::theme
