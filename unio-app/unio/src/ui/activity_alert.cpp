/// @file activity_alert.cpp
/// @brief Stateless renderer for @ref alert::Banner.

#include "ui/activity_alert.hpp"

#include "imgui.h"

#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"

namespace unio_ui::ui::alert {

namespace {

/// @brief (background, foreground) wash for a level.
struct Palette {
    ImVec4 bg;
    ImVec4 fg;
};

Palette wash_for(Level level) {
    switch (level) {
        case Level::Warn:
            return {theme::rgba_hex(0xfdeaea), theme::palette::coral};
        case Level::Info:
        default:
            return {theme::palette::lilac_soft, theme::palette::lilac};
    }
}

}  // namespace

void render(std::optional<Banner>& banner) {
    if (!banner) return;

    const Palette p = wash_for(banner->level);

    const float pad_x = theme::space::md;
    const float pad_y = theme::space::sm;
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float line_h = ImGui::GetTextLineHeight();
    const float box_h  = line_h + 2.0f * pad_y;
    const ImVec2 br(origin.x + avail.x, origin.y + box_h);

    dl->AddRectFilled(origin, br,
                      ImGui::ColorConvertFloat4ToU32(p.bg),
                      theme::radius::sm);

    // Reserve the row in ImGui's layout so subsequent widgets sit
    // below it.
    ImGui::Dummy(ImVec2(avail.x, box_h));

    // Banner text (left-aligned, lilac/coral).
    const ImVec2 text_pos(origin.x + pad_x, origin.y + pad_y);
    ImGui::PushFont(theme::font::body_sm);
    dl->AddText(text_pos,
                ImGui::ColorConvertFloat4ToU32(p.fg),
                banner->text.c_str());
    ImGui::PopFont();

    // Close button — invisible button overlaid on the right edge.
    constexpr float kCloseW = 24.0f;
    const ImVec2 close_tl(br.x - kCloseW, origin.y);
    const ImVec2 close_br(br.x,           origin.y + box_h);

    ImGui::SetCursorScreenPos(close_tl);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0, 0, 0, 0));
    if (ImGui::Button("##alert-close",
                      ImVec2(kCloseW, box_h))) {
        banner.reset();
    }
    ImGui::PopStyleColor(3);

    if (banner) {  // still set — draw the glyph.
        const ImVec2 glyph_size = ImGui::CalcTextSize("\xC3\x97");  // U+00D7 ×
        const ImVec2 glyph_pos(close_br.x - kCloseW * 0.5f - glyph_size.x * 0.5f,
                               origin.y + (box_h - glyph_size.y) * 0.5f);
        dl->AddText(glyph_pos,
                    ImGui::ColorConvertFloat4ToU32(p.fg),
                    "\xC3\x97");
    }

    // Restore the cursor below the banner row + the standard
    // bottom gap. The Dummy() above already advanced the line; this
    // adds the after-banner spacing the surrounding screen relies on.
    ImGui::Dummy(ImVec2(1.0f, theme::space::sm));
}

}  // namespace unio_ui::ui::alert
