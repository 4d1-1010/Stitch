/// @file primitives.cpp
/// @brief Reusable widget primitives.

#include "ui/primitives.hpp"

#include "imgui.h"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"

namespace unio_ui::ui {

namespace {

/// @brief Tint a colour toward black.
constexpr ImVec4 darken(ImVec4 c, float t) {
    return ImVec4(c.x * (1.0f - t),
                  c.y * (1.0f - t),
                  c.z * (1.0f - t),
                  c.w);
}

/// @brief Map @ref DotState to its palette colour.
ImVec4 dot_color(DotState s) {
    switch (s) {
        case DotState::Ok:   return theme::palette::mint;
        case DotState::Warn: return theme::palette::amber;
        case DotState::Bad:  return theme::palette::coral;
        case DotState::Idle: return theme::palette::paper_faint;
    }
    return theme::palette::paper_faint;
}

}  // namespace

bool pill_button(const char* label, PillVariant variant, float /*size_override*/) {
    ImVec4 fill, text, hover, active_fill;
    switch (variant) {
        case PillVariant::Primary:
            fill = theme::palette::lilac;        text = {1, 1, 1, 1};
            hover = theme::palette::lilac_hover;
            active_fill = darken(theme::palette::lilac_hover, 0.1f);
            break;
        case PillVariant::Secondary:
            fill = theme::palette::paper_surface; text = theme::palette::paper_text;
            hover = theme::palette::paper_border; active_fill = theme::palette::paper_rail_deep;
            break;
        case PillVariant::Ghost:
            fill = {0, 0, 0, 0};                  text = theme::palette::paper_text;
            hover = theme::palette::paper_surface; active_fill = theme::palette::paper_border;
            break;
        case PillVariant::Danger:
            fill = theme::palette::coral;         text = {1, 1, 1, 1};
            hover = darken(theme::palette::coral, 0.08f);
            active_fill = darken(theme::palette::coral, 0.15f);
            break;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_fill);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, theme::radius::lg);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(theme::space::lg, theme::space::sm));

    const bool clicked = ImGui::Button(label);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return clicked;
}

void status_dot(DotState state, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float y_mid = pos.y + ImGui::GetTextLineHeight() * 0.5f;
    const float r = size * 0.5f;

    const ImU32 col = ImGui::ColorConvertFloat4ToU32(dot_color(state));
    dl->AddCircleFilled(ImVec2(pos.x + r, y_mid), r, col, 16);
    ImGui::Dummy(ImVec2(size, ImGui::GetTextLineHeight()));
}

void hairline(char axis) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(theme::palette::paper_border);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (axis == 'y') {
        const float h = ImGui::GetContentRegionAvail().y;
        dl->AddLine(p, ImVec2(p.x, p.y + h), col, 1.0f);
        ImGui::Dummy(ImVec2(1.0f, h));
    } else {
        const float w = ImGui::GetContentRegionAvail().x;
        dl->AddLine(p, ImVec2(p.x + w, p.y), col, 1.0f);
        ImGui::Dummy(ImVec2(w, 1.0f));
    }
}

void begin_card(const char* id, ImVec2 size, const ImVec4* accent, float pad) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::paper_surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::radius::md);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));

    ImGui::BeginChild(id, size,
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_None);

    if (accent) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        dl->AddRectFilled(p, ImVec2(p.x + 3.0f, p.y + sz.y),
                          ImGui::ColorConvertFloat4ToU32(*accent),
                          theme::radius::md);
    }
}

void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

}  // namespace unio_ui::ui
