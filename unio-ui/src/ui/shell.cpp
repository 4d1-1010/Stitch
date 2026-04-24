/// @file shell.cpp
/// @brief Root viewport layout and per-tab dispatch.

#include "ui/shell.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/logo.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "ui/activity.hpp"
#include "ui/layout.hpp"
#include "ui/placeholder.hpp"
#include "ui/rail.hpp"

namespace unio_ui::ui {

namespace {

constexpr float kRailWidth        = 108.0f;
constexpr float kRailLogoSize     = 40.0f;
constexpr float kRailLogoPadTop   = 14.0f;
constexpr float kRailLogoPadBot   = 10.0f;
constexpr float kRailButtonHeight = 78.0f;

/// @brief Rail entry descriptor: tab id + caption + vector icon.
struct TabDesc {
    Shell::Tab id;
    const char* label;
    RailIcon icon;
};

constexpr TabDesc kTopTabs[] = {
    {Shell::Tab::Activity, "Activity", RailIcon::Activity},
    {Shell::Tab::Layout,   "Layout",   RailIcon::Layout},
    {Shell::Tab::Settings, "Settings", RailIcon::Settings},
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

    ImGui::Begin("##unio-root", nullptr, kRootFlags);
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
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 p1(p0.x + ImGui::GetWindowWidth(),
                        p0.y + ImGui::GetWindowHeight());
        dl->AddRectFilled(
            p0, p1,
            ImGui::ColorConvertFloat4ToU32(theme::palette::paper_rail),
            theme::radius::md,
            ImDrawFlags_RoundCornersRight);
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

    // Top group (primary nav).
    for (const TabDesc& t : kTopTabs) {
        const bool active = (current_tab_ == t.id);
        if (rail_button(t.label, t.icon, t.label, active)) {
            current_tab_ = t.id;
        }
    }

    // Bottom group pinned to the rail's lower edge.
    constexpr int   kBottomCount = IM_ARRAYSIZE(kBottomTabs);
    const float bottom_block_h = kBottomCount * kRailButtonHeight;
    const float target_y = ImGui::GetWindowHeight()
                         - bottom_block_h - theme::space::sm;
    if (target_y > ImGui::GetCursorPosY()) {
        ImGui::SetCursorPosY(target_y);
    }
    for (const TabDesc& t : kBottomTabs) {
        const bool active = (current_tab_ == t.id);
        if (rail_button(t.label, t.icon, t.label, active)) {
            current_tab_ = t.id;
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void Shell::render_content() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::paper_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(theme::space::xl, theme::space::lg));
    ImGui::BeginChild("##content",
                      ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);

    switch (current_tab_) {
        case Tab::Activity: render_activity(); break;
        case Tab::Layout:   render_layout();   break;
        case Tab::Settings: render_settings(); break;
        case Tab::Access:   render_access();   break;
        case Tab::Help:     render_help();     break;
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Shell::render_activity() { activity::render(orch_); }
void Shell::render_layout()   { layout::render(orch_); }

void Shell::render_settings() {
    placeholder::render(
        "Settings",
        "Local autostart, network interface selection, port, log "
        "folder, and diagnostics will live here.");
}

void Shell::render_access() {
    placeholder::render(
        "Access",
        "Sign in once on any machine in the mesh; other machines "
        "on your LAN are activated automatically.");
}

void Shell::render_help() {
    placeholder::render(
        "Help",
        "Guides, keyboard shortcuts, and troubleshooting tips will "
        "live here.");
}

}  // namespace unio_ui::ui
