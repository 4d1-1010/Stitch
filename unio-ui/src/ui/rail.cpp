/// @file rail.cpp
/// @brief Vector icon drawing + left-rail navigation button.

#include "ui/rail.hpp"

#include "imgui.h"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"

#include <cmath>

namespace unio_ui::ui {

namespace {

/// @brief Activity icon: right-pointing triangle.
void draw_icon_activity(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    dl->AddTriangleFilled(
        ImVec2(c.x - s * 0.30f, c.y - s * 0.35f),
        ImVec2(c.x - s * 0.30f, c.y + s * 0.35f),
        ImVec2(c.x + s * 0.38f, c.y),
        col);
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
        case RailIcon::Activity: draw_icon_activity(dl, c, s, col); break;
        case RailIcon::Layout:   draw_icon_layout  (dl, c, s, col); break;
        case RailIcon::Settings: draw_icon_settings(dl, c, s, col); break;
        case RailIcon::Access:   draw_icon_access  (dl, c, s, col); break;
        case RailIcon::Help:     draw_icon_help    (dl, c, s, col); break;
    }
}

}  // namespace

bool rail_button(const char* id, RailIcon icon,
                 const char* label, bool active) {
    constexpr float kRailWidth = 108.0f;
    constexpr float kHeight    = 78.0f;
    constexpr float kIconSize  = 28.0f;

    const ImVec4 bg      = active ? theme::palette::lilac_soft : theme::palette::paper_rail;
    const ImVec4 bg_h    = active ? theme::palette::lilac_soft : theme::palette::paper_border;
    const ImVec4 text_fg = active ? theme::palette::lilac      : theme::palette::paper_muted;

    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 size(kRailWidth, kHeight);
    const bool clicked = ImGui::InvisibleButton("##btn", size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec4 fill = hovered ? bg_h : bg;
    dl->AddRectFilled(p0,
                      ImVec2(p0.x + size.x, p0.y + size.y),
                      ImGui::ColorConvertFloat4ToU32(fill),
                      theme::radius::sm);

    const ImVec2 icon_center(p0.x + size.x * 0.5f,
                             p0.y + theme::space::md + kIconSize * 0.5f);
    draw_icon(icon, dl, icon_center, kIconSize,
              ImGui::ColorConvertFloat4ToU32(text_fg));

    if (label && label[0] && theme::font::bold) {
        ImFont* const f = theme::font::bold;
        const ImVec2 ls = f->CalcTextSizeA(
            f->LegacySize, FLT_MAX, 0.0f, label);
        const ImVec2 lp(p0.x + (size.x - ls.x) * 0.5f,
                        p0.y + size.y - ls.y - theme::space::sm);
        dl->AddText(f, f->LegacySize, lp,
                    ImGui::ColorConvertFloat4ToU32(text_fg), label);
    }
    ImGui::PopID();
    return clicked;
}

}  // namespace unio_ui::ui
