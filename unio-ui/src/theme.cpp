/*! @file theme.cpp
 *  @brief Paper-lilac design-token impls + ImGui primitives.
 */

#include "theme.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include "font_inter_regular.h"
#include "font_inter_bold.h"

#include <cmath>
#include <cstring>

namespace unio_ui::theme {

namespace font {
ImFont* body = nullptr;
ImFont* body_sm = nullptr;
ImFont* body_xs = nullptr;
ImFont* body_lg = nullptr;
ImFont* bold = nullptr;
ImFont* bold_xs = nullptr;
ImFont* bold_xl = nullptr;
ImFont* title = nullptr;
}  // namespace font

namespace {

/// Tint a colour toward white by @p t in 0..1.
constexpr ImVec4 lighten(ImVec4 c, float t) {
    return ImVec4(
        c.x + (1.0f - c.x) * t,
        c.y + (1.0f - c.y) * t,
        c.z + (1.0f - c.z) * t,
        c.w);
}

/// Tint a colour toward black by @p t in 0..1.
constexpr ImVec4 darken(ImVec4 c, float t) {
    return ImVec4(c.x * (1.0f - t),
                  c.y * (1.0f - t),
                  c.z * (1.0f - t),
                  c.w);
}

ImVec4 dot_color(DotState s) {
    switch (s) {
        case DotState::Ok:   return palette::mint;
        case DotState::Warn: return palette::amber;
        case DotState::Bad:  return palette::coral;
        case DotState::Idle: return palette::paper_faint;
    }
    return palette::paper_faint;
}

ImFont* add_inter(const unsigned char* data, std::size_t size,
                  float px) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // we own the static array
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(data),
        static_cast<int>(size), px, &cfg);
}

}  // namespace

void load_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    // First face added is the default when no PushFont is active.
    // We want `body` (Regular @ size_base) as default.
    font::body    = add_inter(font_inter_regular_data,
                              font_inter_regular_size,
                              font::size_base);
    font::body_sm = add_inter(font_inter_regular_data,
                              font_inter_regular_size,
                              font::size_sm);
    font::body_xs = add_inter(font_inter_regular_data,
                              font_inter_regular_size,
                              font::size_xs);
    font::body_lg = add_inter(font_inter_regular_data,
                              font_inter_regular_size,
                              font::size_lg);
    font::bold    = add_inter(font_inter_bold_data,
                              font_inter_bold_size,
                              font::size_base);
    font::bold_xs = add_inter(font_inter_bold_data,
                              font_inter_bold_size,
                              font::size_xs);
    font::bold_xl = add_inter(font_inter_bold_data,
                              font_inter_bold_size,
                              font::size_xl);
    font::title   = add_inter(font_inter_bold_data,
                              font_inter_bold_size,
                              font::size_title);
    io.FontDefault = font::body;
    // Renderer will call Fonts->Build() in its Init; no explicit
    // build here.
}

void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();

    // ── Rounding ──
    s.WindowRounding    = radius::md;
    s.ChildRounding     = radius::md;
    s.FrameRounding     = radius::sm;
    s.PopupRounding     = radius::md;
    s.ScrollbarRounding = radius::lg;
    s.GrabRounding      = radius::sm;
    s.TabRounding       = radius::sm;

    // ── Spacing ──
    s.WindowPadding      = ImVec2(space::lg, space::lg);
    s.FramePadding       = ImVec2(space::md, space::sm);
    s.ItemSpacing        = ImVec2(space::sm, space::sm);
    s.ItemInnerSpacing   = ImVec2(space::xs, space::xs);
    s.IndentSpacing      = space::lg;
    s.ScrollbarSize      = space::md;
    s.GrabMinSize        = space::md;

    // ── Borders ──
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize  = 1.0f;
    s.PopupBorderSize  = 1.0f;
    s.FrameBorderSize  = 0.0f;
    s.TabBorderSize    = 0.0f;

    // ── Alignment / misc ──
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.ButtonTextAlign  = ImVec2(0.5f, 0.5f);

    // ── Colours ──
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

// ── pill_button ────────────────────────────────────────────────

bool pill_button(const char* label, PillVariant variant, float /*size_override*/) {
    ImVec4 fill, text, hover, active_fill;
    switch (variant) {
        case PillVariant::Primary:
            fill = palette::lilac;         text = {1, 1, 1, 1};
            hover = palette::lilac_hover;  active_fill = darken(palette::lilac_hover, 0.1f);
            break;
        case PillVariant::Secondary:
            fill = palette::paper_surface; text = palette::paper_text;
            hover = palette::paper_border; active_fill = palette::paper_rail_deep;
            break;
        case PillVariant::Ghost:
            fill = {0, 0, 0, 0};           text = palette::paper_text;
            hover = palette::paper_surface; active_fill = palette::paper_border;
            break;
        case PillVariant::Danger:
            fill = palette::coral;         text = {1, 1, 1, 1};
            hover = darken(palette::coral, 0.08f);
            active_fill = darken(palette::coral, 0.15f);
            break;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, fill);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active_fill);
    ImGui::PushStyleColor(ImGuiCol_Text, text);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, radius::lg);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(space::lg, space::sm));

    const bool clicked = ImGui::Button(label);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return clicked;
}

// ── status_dot ─────────────────────────────────────────────────

void status_dot(DotState state, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // Vertically align dot with the current text line.
    const float y_mid = pos.y + ImGui::GetTextLineHeight() * 0.5f;
    const float radius = size * 0.5f;

    const ImU32 col = ImGui::ColorConvertFloat4ToU32(dot_color(state));
    dl->AddCircleFilled(ImVec2(pos.x + radius, y_mid), radius, col, 16);

    // Reserve layout space so the next widget lays out after us.
    ImGui::Dummy(ImVec2(size, ImGui::GetTextLineHeight()));
}

// ── hairline ───────────────────────────────────────────────────

void hairline(char axis) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(palette::paper_border);
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

// ── Rail icons (vector, drawn via ImDrawList) ─────────────────

namespace {

/// Activity: right-pointing triangle.
void draw_icon_activity(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const ImVec2 a(c.x - s * 0.30f, c.y - s * 0.35f);
    const ImVec2 b(c.x - s * 0.30f, c.y + s * 0.35f);
    const ImVec2 t(c.x + s * 0.38f, c.y);
    dl->AddTriangleFilled(a, b, t, col);
}

/// Layout: 2×2 grid of filled squares (represents displays on a desk).
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
                              ImVec2(x + cell, y + cell),
                              col, 2.0f);
        }
    }
}

/// Settings: gear outline + 8 tooth dots around it.
void draw_icon_settings(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    constexpr int kTeeth = 8;
    const float outer = s * 0.42f;
    const float inner = s * 0.28f;
    const float hole  = s * 0.12f;
    for (int i = 0; i < kTeeth; ++i) {
        const float ang = (i / float(kTeeth)) * 2.0f * 3.14159265f;
        const ImVec2 p(c.x + std::cos(ang) * outer,
                       c.y + std::sin(ang) * outer);
        dl->AddCircleFilled(p, s * 0.08f, col, 8);
    }
    dl->AddCircle(c, inner, col, 24, 2.0f);
    dl->AddCircle(c, hole,  col, 12, 2.0f);
}

/// Access: padlock (body + shackle arc).
void draw_icon_access(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    // Body: rounded rectangle centred slightly below c.
    const float body_w = s * 0.60f;
    const float body_h = s * 0.42f;
    const ImVec2 bl(c.x - body_w * 0.5f, c.y);
    const ImVec2 br(c.x + body_w * 0.5f, c.y + body_h);
    dl->AddRectFilled(bl, br, col, 3.0f);

    // Shackle: upper semicircle attached to the body top.
    const float sh_r = s * 0.22f;
    const ImVec2 sh_c(c.x, c.y);
    dl->PathClear();
    dl->PathArcTo(sh_c, sh_r, 3.14159265f, 2.0f * 3.14159265f, 20);
    dl->PathStroke(col, 0, 2.5f);

    // Keyhole dot on the body.
    dl->AddCircleFilled(
        ImVec2(c.x, c.y + body_h * 0.45f),
        s * 0.05f,
        IM_COL32(0xf4, 0xf5, 0xf8, 0xff),
        8);
}

/// Help: circled "?" using the bundled Inter Bold face at xl size.
void draw_icon_help(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    dl->AddCircle(c, s * 0.42f, col, 32, 2.0f);
    // "?" glyph centred. Inter covers basic Latin, so the atlas
    // has it.
    ImFont* f = font::bold_xl ? font::bold_xl : ImGui::GetFont();
    const char* q = "?";
    const ImVec2 ts = f->CalcTextSizeA(f->LegacySize, FLT_MAX, 0.0f, q);
    dl->AddText(f, f->LegacySize,
                ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                col, q);
}

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

// ── rail_button ────────────────────────────────────────────────

bool rail_button(const char* id, RailIcon icon,
                 const char* label, bool active) {
    // Layout is a fixed-width vertical stack: glyph-line on top,
    // label below. Tk impl used 108 px rail width; mirror that.
    constexpr float kRailWidth = 108.0f;
    constexpr float kHeight = 78.0f;

    const ImVec4 bg      = active ? palette::lilac_soft : palette::paper_rail;
    const ImVec4 bg_h    = active ? palette::lilac_soft : palette::paper_border;
    const ImVec4 text_fg = active ? palette::lilac      : palette::paper_muted;

    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_h);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette::lilac_soft);
    ImGui::PushStyleColor(ImGuiCol_Text, text_fg);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, radius::sm);
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));

    // We want a centred two-line composition. The cleanest ImGui
    // pattern is an invisible button sized to the target, then
    // overlay-draw the glyph + label in the centred region.
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
                      radius::sm);

    // Vector icon, centred horizontally in the upper third.
    constexpr float kIconSize = 28.0f;
    const ImVec2 icon_center(p0.x + size.x * 0.5f,
                             p0.y + space::md + kIconSize * 0.5f);
    draw_icon(icon, dl, icon_center, kIconSize,
              ImGui::ColorConvertFloat4ToU32(text_fg));

    if (label && label[0] && font::bold_xs) {
        const ImVec2 ls = font::bold_xs->CalcTextSizeA(
            font::bold_xs->LegacySize, FLT_MAX, 0.0f, label);
        const ImVec2 lp(p0.x + (size.x - ls.x) * 0.5f,
                        p0.y + size.y - ls.y - space::sm);
        dl->AddText(font::bold_xs, font::bold_xs->LegacySize, lp,
                    ImGui::ColorConvertFloat4ToU32(text_fg), label);
    }
    ImGui::PopID();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    return clicked;
}

// ── card ───────────────────────────────────────────────────────

void begin_card(const char* id, ImVec2 size,
                const ImVec4* accent, float pad) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette::paper_surface);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, radius::md);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));

    ImGui::BeginChild(id, size,
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_None);

    if (accent) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        dl->AddRectFilled(
            p, ImVec2(p.x + 3.0f, p.y + sz.y),
            ImGui::ColorConvertFloat4ToU32(*accent),
            radius::md);
    }
}

void end_card() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

}  // namespace unio_ui::theme
