/// @file style.cpp
/// @brief Font atlas loader + ImGui-style application.

#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/style.hpp"
#include "theme/typography.hpp"

#include "imgui.h"

#include "font_inter_regular.h"
#include "font_inter_bold.h"

#include <cstddef>

namespace xorio_ui::theme {

namespace font {
ImFont* body    = nullptr;
ImFont* body_sm = nullptr;
ImFont* body_xs = nullptr;
ImFont* body_lg = nullptr;
ImFont* bold    = nullptr;
ImFont* bold_xs = nullptr;
ImFont* bold_xl = nullptr;
ImFont* title   = nullptr;
}  // namespace font

namespace {

/// @brief Register one face from an embedded TTF blob.
ImFont* add_inter(const unsigned char* data, std::size_t size, float px) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(data),
        static_cast<int>(size), px, &cfg);
}

/// @brief Tint a colour toward white.
constexpr ImVec4 lighten(ImVec4 c, float t) {
    return ImVec4(c.x + (1.0f - c.x) * t,
                  c.y + (1.0f - c.y) * t,
                  c.z + (1.0f - c.z) * t,
                  c.w);
}

/// @brief Tint a colour toward black.
constexpr ImVec4 darken(ImVec4 c, float t) {
    return ImVec4(c.x * (1.0f - t),
                  c.y * (1.0f - t),
                  c.z * (1.0f - t),
                  c.w);
}

}  // namespace

void load_fonts() {
    font::body    = add_inter(font_inter_regular_data, font_inter_regular_size, font::size_base);
    font::body_sm = add_inter(font_inter_regular_data, font_inter_regular_size, font::size_sm);
    font::body_xs = add_inter(font_inter_regular_data, font_inter_regular_size, font::size_xs);
    font::body_lg = add_inter(font_inter_regular_data, font_inter_regular_size, font::size_lg);
    font::bold    = add_inter(font_inter_bold_data,    font_inter_bold_size,    font::size_base);
    font::bold_xs = add_inter(font_inter_bold_data,    font_inter_bold_size,    font::size_xs);
    font::bold_xl = add_inter(font_inter_bold_data,    font_inter_bold_size,    font::size_xl);
    font::title   = add_inter(font_inter_bold_data,    font_inter_bold_size,    font::size_title);
    ImGui::GetIO().FontDefault = font::body;
}

void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = radius::md;
    s.ChildRounding     = radius::md;
    s.FrameRounding     = radius::sm;
    s.PopupRounding     = radius::md;
    s.ScrollbarRounding = radius::lg;
    s.GrabRounding      = radius::sm;
    s.TabRounding       = radius::sm;

    s.WindowPadding    = ImVec2(space::lg, space::lg);
    s.FramePadding     = ImVec2(space::md, space::sm);
    s.ItemSpacing      = ImVec2(space::sm, space::sm);
    s.ItemInnerSpacing = ImVec2(space::xs, space::xs);
    s.IndentSpacing    = space::lg;
    s.ScrollbarSize    = space::md;
    s.GrabMinSize      = space::md;

    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize  = 1.0f;
    s.PopupBorderSize  = 1.0f;
    s.FrameBorderSize  = 0.0f;
    s.TabBorderSize    = 0.0f;

    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.ButtonTextAlign  = ImVec2(0.5f, 0.5f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = palette::paper_text;
    c[ImGuiCol_TextDisabled]          = palette::paper_faint;
    c[ImGuiCol_WindowBg]              = palette::paper_bg;
    c[ImGuiCol_ChildBg]               = palette::paper_surface;
    c[ImGuiCol_PopupBg]               = palette::paper_bg;
    c[ImGuiCol_Border]                = palette::paper_border;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = palette::paper_surface;
    c[ImGuiCol_FrameBgHovered]        = lighten(palette::lilac, 0.85f);
    c[ImGuiCol_FrameBgActive]         = palette::lilac_soft;
    c[ImGuiCol_TitleBg]               = palette::paper_rail;
    c[ImGuiCol_TitleBgActive]         = palette::paper_rail_deep;
    c[ImGuiCol_TitleBgCollapsed]      = palette::paper_rail;
    c[ImGuiCol_MenuBarBg]             = palette::paper_rail;
    c[ImGuiCol_ScrollbarBg]           = palette::paper_bg;
    c[ImGuiCol_ScrollbarGrab]         = palette::paper_border;
    c[ImGuiCol_ScrollbarGrabHovered]  = palette::paper_faint;
    c[ImGuiCol_ScrollbarGrabActive]   = palette::paper_muted;
    c[ImGuiCol_CheckMark]             = palette::lilac;
    c[ImGuiCol_SliderGrab]            = palette::lilac;
    c[ImGuiCol_SliderGrabActive]      = palette::lilac_hover;
    c[ImGuiCol_Button]                = palette::paper_surface;
    c[ImGuiCol_ButtonHovered]         = palette::paper_border;
    c[ImGuiCol_ButtonActive]          = palette::paper_rail_deep;
    c[ImGuiCol_Header]                = palette::lilac_soft;
    c[ImGuiCol_HeaderHovered]         = lighten(palette::lilac, 0.75f);
    c[ImGuiCol_HeaderActive]          = palette::lilac;
    c[ImGuiCol_Separator]             = palette::paper_border;
    c[ImGuiCol_SeparatorHovered]      = palette::paper_muted;
    c[ImGuiCol_SeparatorActive]       = palette::paper_text;
    c[ImGuiCol_ResizeGrip]            = palette::paper_border;
    c[ImGuiCol_ResizeGripHovered]     = palette::paper_muted;
    c[ImGuiCol_ResizeGripActive]      = palette::lilac;
    c[ImGuiCol_Tab]                   = palette::paper_rail;
    c[ImGuiCol_TabHovered]            = palette::lilac_soft;
    c[ImGuiCol_TabSelected]           = palette::paper_bg;
    c[ImGuiCol_TabDimmed]             = palette::paper_rail;
    c[ImGuiCol_TabDimmedSelected]     = palette::paper_surface;
    c[ImGuiCol_PlotLines]             = palette::lilac;
    c[ImGuiCol_PlotLinesHovered]      = palette::lilac_hover;
    c[ImGuiCol_PlotHistogram]         = palette::mint;
    c[ImGuiCol_PlotHistogramHovered]  = darken(palette::mint, 0.1f);
    c[ImGuiCol_TableHeaderBg]         = palette::paper_rail;
    c[ImGuiCol_TableBorderStrong]     = palette::paper_border;
    c[ImGuiCol_TableBorderLight]      = palette::paper_border;
    c[ImGuiCol_TableRowBg]            = palette::paper_bg;
    c[ImGuiCol_TableRowBgAlt]         = palette::paper_surface;
    c[ImGuiCol_TextSelectedBg]        = palette::lilac_soft;
    c[ImGuiCol_DragDropTarget]        = palette::lilac;
    c[ImGuiCol_NavCursor]             = palette::lilac;
    c[ImGuiCol_NavWindowingHighlight] = palette::lilac;
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.35f);
}

}  // namespace xorio_ui::theme
