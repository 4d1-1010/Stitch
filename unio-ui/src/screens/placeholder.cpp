/*! @file placeholder.cpp
 *  @brief Shared "coming soon" screen.
 */

#include "placeholder.hpp"

#include "imgui.h"

#include "../theme.hpp"

namespace unio_ui::screens::placeholder {

namespace {

/// Centre a block of @p width px in the remaining content region.
void center_cursor_x(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float pad = (avail - width) * 0.5f;
    if (pad > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    }
}

}  // namespace

void render(const char* title, const char* body) {
    // Same vertical offset as the Activity alone-state so the
    // three tabs feel visually cousins.
    constexpr float kTopOffset = 2.0f * 48.0f;
    constexpr float kWrapWidth = 520.0f;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + kTopOffset);

    // ── Title ────────────────────────────────────────────────
    ImGui::PushFont(theme::font::title);
    const float tw = ImGui::CalcTextSize(title).x;
    center_cursor_x(tw);
    ImGui::TextColored(theme::palette::paper_text, "%s", title);
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Spacing();

    // ── Body (wrapped, centred block) ────────────────────────
    center_cursor_x(kWrapWidth);
    const float start_x = ImGui::GetCursorPosX();
    ImGui::PushTextWrapPos(start_x + kWrapWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::paper_muted);
    ImGui::TextUnformatted(body);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    // ── "Coming soon" tag ────────────────────────────────────
    ImGui::PushFont(theme::font::body_sm);
    const char* tag = "COMING SOON";
    const float sw = ImGui::CalcTextSize(tag).x;
    center_cursor_x(sw);
    ImGui::TextColored(theme::palette::lilac, "%s", tag);
    ImGui::PopFont();
}

}  // namespace unio_ui::screens::placeholder
