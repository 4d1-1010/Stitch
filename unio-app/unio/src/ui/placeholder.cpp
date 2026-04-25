/// @file placeholder.cpp
/// @brief Centred "coming soon" screen shared by unimplemented tabs.

#include "ui/placeholder.hpp"

#include "imgui.h"

#include "theme/palette.hpp"
#include "theme/typography.hpp"

#include <cstring>

namespace unio_ui::ui::placeholder {

namespace {

/// @brief Offset the cursor so a @p width-pixel block sits centred
/// in the remaining content region.
void center_cursor_x(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float pad = (avail - width) * 0.5f;
    if (pad > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    }
}

/// @brief Word-wrap @p text to @p wrap_width and centre each line.
void render_wrapped_centered(const char* text, float wrap_width,
                             ImVec4 color) {
    ImFont* font = ImGui::GetFont();
    const float font_size = font->LegacySize;
    const char* const text_end = text + std::strlen(text);
    const char* p = text;
    while (p <= text_end) {
        const char* para_end = p;
        while (para_end < text_end && *para_end != '\n') ++para_end;
        if (p == para_end) {
            ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
        } else {
            const char* line = p;
            while (line < para_end) {
                const char* line_end = font->CalcWordWrapPosition(
                    font_size, line, para_end, wrap_width);
                if (line_end == line) line_end = line + 1;
                const ImVec2 sz = font->CalcTextSizeA(
                    font_size, FLT_MAX, 0.0f, line, line_end);
                center_cursor_x(sz.x);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(line, line_end);
                ImGui::PopStyleColor();
                line = line_end;
                while (line < para_end && *line == ' ') ++line;
            }
        }
        if (para_end == text_end) break;
        p = para_end + 1;
    }
}

}  // namespace

void render(const char* title, const char* body) {
    constexpr float kTopOffset = 2.0f * 48.0f;
    constexpr float kWrapWidth = 520.0f;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + kTopOffset);

    ImGui::PushFont(theme::font::title);
    const float tw = ImGui::CalcTextSize(title).x;
    center_cursor_x(tw);
    ImGui::TextColored(theme::palette::paper_text, "%s", title);
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Spacing();

    render_wrapped_centered(body, kWrapWidth, theme::palette::paper_muted);

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::PushFont(theme::font::body_sm);
    const char* tag = "COMING SOON";
    const float sw = ImGui::CalcTextSize(tag).x;
    center_cursor_x(sw);
    ImGui::TextColored(theme::palette::lilac, "%s", tag);
    ImGui::PopFont();
}

}  // namespace unio_ui::ui::placeholder
