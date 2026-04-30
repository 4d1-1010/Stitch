/// @file shell.cpp
/// @brief Root viewport layout and per-tab dispatch.

#include "ui/shell.hpp"

#include "imgui.h"

#include <algorithm>

#include "orchestrator/orchestrator.hpp"
#include "theme/logo.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "ui/access.hpp"
#include "ui/workspaces/tab.hpp"
#include "ui/layout.hpp"
#include "ui/placeholder.hpp"
#include "ui/rail.hpp"

namespace xorio::ui {

namespace {

// Rail width and button height live in ui/rail.hpp so the button
// renderer and the shell agree. Logo metrics are shell-only.
constexpr float kRailLogoSize     = 64.0f;
constexpr float kRailLogoPadTop   = 18.0f;
constexpr float kRailLogoPadBot   = 14.0f;

/// @brief Rail entry descriptor: tab id + caption + vector icon.
struct TabDesc {
    Shell::Tab id;
    const char* label;
    RailIcon icon;
};

constexpr TabDesc kTopTabs[] = {
    {Shell::Tab::Workspaces, "Workspaces", RailIcon::Workspaces},
    {Shell::Tab::Layout,   "Layout",     RailIcon::Layout},
    {Shell::Tab::LiveView, "Live view",  RailIcon::LiveView},
    {Shell::Tab::Settings, "Settings",   RailIcon::Settings},
};

constexpr TabDesc kBottomTabs[] = {
    {Shell::Tab::Access, "Access", RailIcon::Access},
    {Shell::Tab::Help,   "Help",   RailIcon::Help},
};

}  // namespace

Shell::Shell(orchestrator::IOrchestrator& orch) : orch_(orch) {}

void Shell::render() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    constexpr ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::Begin("##xorio-root", nullptr, kRootFlags);
    render_rail();
    ImGui::SameLine(0.0f, 0.0f);
    render_content();
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void Shell::render_rail() {
    // Rail background is drawn manually with only the right-side
    // corners rounded (the left edge hugs the window border).
    // Child is transparent + unrounded; we fill a rounded rect
    // via the draw list before rendering any content.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(0.0f, theme::space::sm));
    ImGui::BeginChild("##rail",
                      ImVec2(kRailWidth, 0.0f),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    {
        // Rail body uses a multi-stop vertical lilac gradient with
        // intermediate stops at each nav button's centre. Right
        // corners protrude outward by r pixels with a *concave*
        // outer arc (the curve dips back toward the rail so the
        // outline reads as a fillet from the page side). A thin
        // lilac stripe traces the rail's right outline only — no
        // top, right or bottom edge of the content area.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1(p0.x + ImGui::GetWindowWidth(),
                        p0.y + ImGui::GetWindowHeight());
        const ImU32 rail_top =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_top);
        const ImU32 rail_bot =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_bot);
        const ImU32 col_activity =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_workspaces);
        const ImU32 col_layout =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_layout);
        const ImU32 col_settings =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_settings);
        const ImU32 col_access =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_access);
        const ImU32 col_help =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_help);
        const ImU32 page_col =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_bg);
        const ImU32 edge_col =
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail_edge);
        const float r           = theme::radius::md;
        constexpr float kPi      = 3.14159265f;
        constexpr float kStripeW = 2.0f;

        // Y positions of the gradient's intermediate stops — each
        // sits at the centre of the matching nav button.
        const float y_button_h    = kRailButtonHeight;
        const float y_top_group_0 = p0.y + theme::space::sm
                                  + kRailLogoPadTop
                                  + kRailLogoSize + kRailLogoPadBot;
        const float y_activity_c  = y_top_group_0 + y_button_h * 0.5f;
        const float y_layout_c    = y_top_group_0 + y_button_h * 1.5f;
        const float y_settings_c  = y_top_group_0 + y_button_h * 2.5f;
        const float y_bot_group_0 = p1.y - 2.0f * y_button_h - theme::space::sm;
        const float y_access_c    = y_bot_group_0 + y_button_h * 0.5f;
        const float y_help_c      = y_bot_group_0 + y_button_h * 1.5f;

        // Multi-stop gradient over the rail's bounds.
        auto seg = [&](float y_a, float y_b, ImU32 c_a, ImU32 c_b) {
            dl->AddRectFilledMultiColor(
                ImVec2(p0.x, y_a), ImVec2(p1.x, y_b),
                c_a, c_a, c_b, c_b);
        };
        seg(p0.y,         y_activity_c, rail_top,     col_activity);
        seg(y_activity_c, y_layout_c,   col_activity, col_layout);
        seg(y_layout_c,   y_settings_c, col_layout,   col_settings);
        seg(y_settings_c, y_access_c,   col_settings, col_access);
        seg(y_access_c,   y_help_c,     col_access,   col_help);
        seg(y_help_c,     p1.y,         col_help,     rail_bot);

        // The rail's child window clips drawing at x = p1.x; widen
        // so the corner extensions and stripe past p1.x render.
        dl->PushClipRect(ImVec2(p0.x, p0.y),
                         ImVec2(p1.x + r + 2.0f, p1.y),
                         /*intersect_with_current=*/false);

        // Top-right corner extension + concave-fillet carve.
        dl->AddRectFilled(ImVec2(p1.x, p0.y),
                          ImVec2(p1.x + r, p0.y + r),
                          rail_top);
        dl->PathClear();
        dl->PathLineTo(ImVec2(p1.x + r, p0.y + r));
        dl->PathArcTo(ImVec2(p1.x + r, p0.y + r), r, kPi, kPi * 1.5f);
        dl->PathFillConvex(page_col);

        // Bottom-right corner extension + concave-fillet carve.
        dl->AddRectFilled(ImVec2(p1.x, p1.y - r),
                          ImVec2(p1.x + r, p1.y),
                          rail_bot);
        dl->PathClear();
        dl->PathLineTo(ImVec2(p1.x + r, p1.y - r));
        dl->PathArcTo(ImVec2(p1.x + r, p1.y - r), r, kPi * 0.5f, kPi);
        dl->PathFillConvex(page_col);

        // Lilac accent stripe — traces only the rail's right
        // outline (top concave fillet, straight middle, bottom
        // concave fillet). No top, right, or bottom edge of the
        // content area.
        dl->PathClear();
        // Top concave fillet: outer top corner of extension →
        // straight right edge.
        dl->PathArcTo(ImVec2(p1.x + r, p0.y + r), r, kPi * 1.5f, kPi);
        // Bottom concave fillet: straight right edge → outer
        // bottom corner of extension.
        dl->PathArcTo(ImVec2(p1.x + r, p1.y - r), r, kPi, kPi * 0.5f);
        dl->PathStroke(edge_col, ImDrawFlags_None, kStripeW);

        dl->PopClipRect();
    }

    // Logo at the rail head.
    const auto& logo = theme::logo_texture();
    ImGui::Dummy(ImVec2(kRailWidth, kRailLogoPadTop));
    if (logo.texture) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float x = p.x + (kRailWidth - kRailLogoSize) * 0.5f;
        dl->AddImage(logo.texture,
                     ImVec2(x, p.y),
                     ImVec2(x + kRailLogoSize, p.y + kRailLogoSize));
    }
    ImGui::Dummy(ImVec2(kRailWidth, kRailLogoSize + kRailLogoPadBot));

    // While the access gate is closed every other tab is locked
    // out; only the Access entry stays clickable. Visually the
    // locked tabs render at reduced alpha so the rail still
    // communicates the eventual layout.
    const bool unlocked = orch_.access_authorized();

    // Top group (primary nav).
    for (const TabDesc& t : kTopTabs) {
        const bool active   = (current_tab_ == t.id);
        const bool disabled = !unlocked;
        if (disabled) ImGui::BeginDisabled();
        if (rail_button(t.label, t.icon, t.label, active)) {
            current_tab_ = t.id;
        }
        if (disabled) ImGui::EndDisabled();
    }

    // Bottom group pinned to the rail's lower edge.
    constexpr int kBottomCount  = IM_ARRAYSIZE(kBottomTabs);
    const float bottom_block_h  = kBottomCount * kRailButtonHeight;
    const float target_y = ImGui::GetWindowHeight()
                         - bottom_block_h - theme::space::sm;
    if (target_y > ImGui::GetCursorPosY()) {
        ImGui::SetCursorPosY(target_y);
    }
    for (const TabDesc& t : kBottomTabs) {
        const bool active   = (current_tab_ == t.id);
        // Access stays interactive at all times; Help is part of
        // the locked set until the gate opens.
        const bool disabled = !unlocked && t.id != Tab::Access;
        if (disabled) ImGui::BeginDisabled();
        if (rail_button(t.label, t.icon, t.label, active)) {
            current_tab_ = t.id;
        }
        if (disabled) ImGui::EndDisabled();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void Shell::render_content() {
    // Force-route to the Access tab while the gate is closed. The
    // rail's other tabs are also disabled, but if the user lands
    // here mid-state (e.g. tab restored from a previous session)
    // we still flip the body to Access on the next frame.
    if (!orch_.access_authorized()) {
        current_tab_ = Tab::Access;
    }

    // The content area has two children:
    //   * `##content`  — outer paper-bg surface that fills every
    //                    pixel between the rail and the right
    //                    edge of the window.
    //   * `##page`     — inner transparent child with explicit
    //                    inset width so the tab body has visibly
    //                    equal left + right + top + bottom
    //                    margins. Without this nesting the body
    //                    runs flush to the right edge while the
    //                    left side is indented by Indent(), which
    //                    reads as a lopsided canvas.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::paper_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##content",
                      ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar
                          | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 outer = ImGui::GetContentRegionAvail();
    const float  page_x = theme::space::page_x;
    const float  page_y = theme::space::page_y;
    const float  inner_w = std::max(1.0f, outer.x - 2.0f * page_x);
    const float  inner_h = std::max(1.0f, outer.y - 2.0f * page_y);

    ImGui::SetCursorPos(ImVec2(page_x, page_y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::BeginChild("##page",
                      ImVec2(inner_w, inner_h),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar
                          | ImGuiWindowFlags_NoScrollWithMouse);

    switch (current_tab_) {
        case Tab::Workspaces: render_workspaces();  break;
        case Tab::Layout:   render_layout();    break;
        case Tab::LiveView: render_live_view(); break;
        case Tab::Settings: render_settings();  break;
        case Tab::Access:   render_access();    break;
        case Tab::Help:     render_help();      break;
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Shell::render_workspaces() { workspaces::render_tab(orch_); }
void Shell::render_layout()   { layout::render(orch_); }

void Shell::render_live_view() {
    placeholder::render(
        "Live view",
        "Real-time monitoring of every connected display will live "
        "here — watch what each computer is doing without leaving "
        "your seat.");
}

void Shell::render_settings() {
    placeholder::render(
        "Settings",
        "Local autostart, network interface selection, port, log "
        "folder, and diagnostics will live here.");
}

void Shell::render_access() { access::render(orch_); }

void Shell::render_help() {
    placeholder::render(
        "Help",
        "Guides, keyboard shortcuts, and troubleshooting tips will "
        "live here.");
}

}  // namespace xorio::ui
