/// @file rail.cpp
/// @brief Vector icon drawing + left-rail navigation button.

#include "ui/rail.hpp"

#include "imgui.h"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"

#include <cmath>

namespace xorio::ui {

namespace {

/// @brief Workspaces icon: three stacked horizontal pills, each
/// with a leading dot — reads as a list of grouped workspace cards.
void draw_icon_workspaces(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    constexpr int   kRows    = 3;
    const float     bar_h    = s * 0.13f;
    const float     bar_w    = s * 0.78f;
    const float     gap_y    = s * 0.10f;
    const float     dot_r    = s * 0.055f;
    const float     dot_pad  = s * 0.10f;
    const float     total_h  = kRows * bar_h + (kRows - 1) * gap_y;

    const float x0 = c.x - bar_w * 0.5f;
    float       y  = c.y - total_h * 0.5f;
    for (int i = 0; i < kRows; ++i) {
        // Bar (rounded pill).
        dl->AddRectFilled(
            ImVec2(x0, y),
            ImVec2(x0 + bar_w, y + bar_h),
            col, bar_h * 0.5f);
        // Leading dot — punched out of the bar so the pill reads
        // as a row containing a marker.
        dl->AddCircleFilled(
            ImVec2(x0 + dot_pad + dot_r, y + bar_h * 0.5f),
            dot_r,
            IM_COL32(0xff, 0xff, 0xff, 0xff),
            10);
        y += bar_h + gap_y;
    }
}

/// @brief Layout icon: 2×2 grid of filled squares.
void draw_icon_layout(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const float cell = s * 0.30f;
    const float gap  = s * 0.08f;
    const float x0 = c.x - cell - gap * 0.5f;
    const float y0 = c.y - cell - gap * 0.5f;
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            const float x = x0 + i * (cell + gap);
            const float y = y0 + j * (cell + gap);
            dl->AddRectFilled(ImVec2(x, y),
                              ImVec2(x + cell, y + cell), col, 2.0f);
        }
    }
}

/// @brief Settings icon: ring with eight peripheral dots.
void draw_icon_settings(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    constexpr int kTeeth = 8;
    const float outer = s * 0.42f;
    const float inner = s * 0.28f;
    const float hole  = s * 0.12f;
    for (int i = 0; i < kTeeth; ++i) {
        const float ang = (i / float(kTeeth)) * 2.0f * 3.14159265f;
        dl->AddCircleFilled(
            ImVec2(c.x + std::cos(ang) * outer,
                   c.y + std::sin(ang) * outer),
            s * 0.08f, col, 8);
    }
    dl->AddCircle(c, inner, col, 24, 2.0f);
    dl->AddCircle(c, hole,  col, 12, 2.0f);
}

/// @brief Access icon: padlock (body + shackle arc + keyhole dot).
void draw_icon_access(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const float body_w = s * 0.60f;
    const float body_h = s * 0.42f;
    dl->AddRectFilled(
        ImVec2(c.x - body_w * 0.5f, c.y),
        ImVec2(c.x + body_w * 0.5f, c.y + body_h),
        col, 3.0f);
    dl->PathClear();
    dl->PathArcTo(c, s * 0.22f, 3.14159265f, 2.0f * 3.14159265f, 20);
    dl->PathStroke(col, 0, 2.5f);
    dl->AddCircleFilled(
        ImVec2(c.x, c.y + body_h * 0.45f),
        s * 0.05f,
        IM_COL32(0xf4, 0xf5, 0xf8, 0xff), 8);
}

/// @brief Live view icon: monitor frame with a small "live" dot in
/// the upper-right corner.
void draw_icon_live_view(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const float w = s * 0.74f;
    const float h = s * 0.52f;
    const float x0 = c.x - w * 0.5f;
    const float y0 = c.y - h * 0.5f;
    // Screen frame (rounded outline).
    dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h),
                col, s * 0.06f, ImDrawFlags_None, 2.0f);
    // Stand: short vertical + horizontal foot.
    dl->AddLine(ImVec2(c.x, y0 + h),
                ImVec2(c.x, y0 + h + s * 0.10f), col, 2.0f);
    dl->AddLine(ImVec2(c.x - s * 0.18f, y0 + h + s * 0.10f),
                ImVec2(c.x + s * 0.18f, y0 + h + s * 0.10f), col, 2.0f);
    // Live indicator — small filled dot inside the screen, top-right.
    dl->AddCircleFilled(
        ImVec2(x0 + w - s * 0.12f, y0 + s * 0.12f),
        s * 0.06f, col, 12);
}

/// @brief Help icon: circled "?" glyph.
void draw_icon_help(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    dl->AddCircle(c, s * 0.42f, col, 32, 2.0f);
    ImFont* f = theme::font::bold_xl ? theme::font::bold_xl : ImGui::GetFont();
    const char* q = "?";
    const ImVec2 ts = f->CalcTextSizeA(f->LegacySize, FLT_MAX, 0.0f, q);
    dl->AddText(f, f->LegacySize,
                ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                col, q);
}

/// @brief Dispatch to the icon-specific drawer.
void draw_icon(RailIcon kind, ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    switch (kind) {
        case RailIcon::Workspaces: draw_icon_workspaces (dl, c, s, col); break;
        case RailIcon::Layout:   draw_icon_layout   (dl, c, s, col); break;
        case RailIcon::LiveView: draw_icon_live_view(dl, c, s, col); break;
        case RailIcon::Settings: draw_icon_settings (dl, c, s, col); break;
        case RailIcon::Access:   draw_icon_access   (dl, c, s, col); break;
        case RailIcon::Help:     draw_icon_help     (dl, c, s, col); break;
    }
}

}  // namespace

bool rail_button(const char* id, RailIcon icon,
                 const char* label, bool active) {
    constexpr float kIconSize     = 24.0f;
    constexpr float kIconLabelGap = 12.0f;
    constexpr float kPadX         = 24.0f;

    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(kRailWidth, kRailButtonHeight);
    const bool clicked = ImGui::InvisibleButton("##btn", size);
    const bool hovered = ImGui::IsItemHovered();

    // Buttons are transparent on the lilac rail — active / hover
    // are signalled through the icon + label colour only.
    const ImVec4 text_fg = active  ? theme::palette::lilac
                         : hovered ? theme::palette::paper_text
                                   : theme::palette::paper_muted;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Selected tab gets a rounded-rect background pill in a soft
    // lilac. Inactive buttons stay transparent (active state is
    // shown by both the pill + the text/icon colour shift).
    if (active) {
        constexpr float kBgInsetX = 8.0f;
        constexpr float kBgInsetY = 4.0f;
        dl->AddRectFilled(
            ImVec2(p0.x + kBgInsetX, p0.y + kBgInsetY),
            ImVec2(p0.x + size.x - kBgInsetX, p0.y + size.y - kBgInsetY),
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_active),
            theme::radius::md);
    }

    // Horizontal layout: icon at a fixed left position, label
    // starting at a fixed offset after it — so icons line up in
    // a column across all buttons and labels share a common left
    // edge. Both vertically centred in the row.
    const float row_cy = p0.y + size.y * 0.5f;
    const ImU32 fg_col = ImGui::ColorConvertFloat4ToU32(text_fg);
    const ImVec2 icon_center(p0.x + kPadX + kIconSize * 0.5f, row_cy);
    draw_icon(icon, dl, icon_center, kIconSize, fg_col);

    if (label && label[0] && theme::font::bold) {
        ImFont* const f = theme::font::bold;
        const ImVec2 ls = f->CalcTextSizeA(
            f->LegacySize, FLT_MAX, 0.0f, label);
        const ImVec2 lp(p0.x + kPadX + kIconSize + kIconLabelGap,
                        row_cy - ls.y * 0.5f);
        dl->AddText(f, f->LegacySize, lp, fg_col, label);
    }
    ImGui::PopID();
    return clicked;
}

}  // namespace xorio::ui
