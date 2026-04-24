/*! @file shell.cpp
 *  @brief Main-window layout implementation.
 */

#include "shell.hpp"

#include "imgui.h"

#include "../orchestrator.hpp"
#include "../theme.hpp"
#include "activity.hpp"
#include "layout.hpp"
#include "placeholder.hpp"

#include <cstdio>

namespace unio_ui::screens {

namespace {

constexpr float kRailWidth = 108.0f;
constexpr float kTopBarHeight = 56.0f;

struct TabDesc {
    Shell::Tab id;
    const char* label;
    theme::RailIcon icon;
};

// Top-rail group — primary navigation.
constexpr TabDesc kTopTabs[] = {
    {Shell::Tab::Activity, "Activity", theme::RailIcon::Activity},
    {Shell::Tab::Layout,   "Layout",   theme::RailIcon::Layout},
    {Shell::Tab::Settings, "Settings", theme::RailIcon::Settings},
};

// Bottom-rail group — secondary / utility entries. Anchored to
// the rail's bottom edge. Matches the common desktop-app pattern
// (Access / Help are not daily nav; primary flow stays at the
// top where the eye lands first).
constexpr TabDesc kBottomTabs[] = {
    {Shell::Tab::Access, "Access", theme::RailIcon::Access},
    {Shell::Tab::Help,   "Help",   theme::RailIcon::Help},
};

constexpr float kRailButtonHeight = 78.0f;  // matches theme::rail_button

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

    render_top_bar();
    render_rail();
    ImGui::SameLine(0.0f, 0.0f);
    render_content();

    ImGui::End();
    ImGui::PopStyleVar(3);
}

void Shell::render_top_bar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::paper_rail_deep);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(theme::space::lg, theme::space::md));
    ImGui::BeginChild("##topbar",
                      ImVec2(0.0f, kTopBarHeight),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    // Wordmark — "un" lilac / "IO" paper_text, bold title size.
    ImGui::PushFont(theme::font::title);
    ImGui::TextColored(theme::palette::lilac, "un");
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(theme::palette::paper_text, "IO");
    ImGui::PopFont();

    // Right-anchored status: <dot> <state-label> · <hostname>.
    const auto astate = orch_.auth_state();
    const char* state_label = (astate == orchestrator::AuthState::SignedIn)
                              ? "online"
                              : (astate == orchestrator::AuthState::GracePeriod)
                                ? "searching" : "offline";
    const theme::DotState dot_state =
        (astate == orchestrator::AuthState::SignedIn)   ? theme::DotState::Ok
        : (astate == orchestrator::AuthState::GracePeriod) ? theme::DotState::Warn
        :                                                 theme::DotState::Bad;

    char right_label[128];
    std::snprintf(right_label, sizeof(right_label), "%s · %s",
                  state_label, orch_.local_display_name().c_str());

    ImGui::PushFont(theme::font::body_sm);
    const float right_w = ImGui::CalcTextSize(right_label).x + 20.0f;
    ImGui::PopFont();

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - right_w);
    theme::status_dot(dot_state);
    ImGui::SameLine();
    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_muted, "%s", right_label);
    ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Shell::render_rail() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::paper_rail);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(0.0f, theme::space::sm));
    ImGui::BeginChild("##rail",
                      ImVec2(kRailWidth, 0.0f),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    // Top group.
    for (const TabDesc& t : kTopTabs) {
        const bool active = (current_tab_ == t.id);
        if (theme::rail_button(t.label, t.icon, t.label, active)) {
            current_tab_ = t.id;
        }
    }

    // Bottom group — anchored to the rail's lower edge so Access
    // and Help sit at the bottom regardless of window height.
    constexpr int kBottomCount = IM_ARRAYSIZE(kBottomTabs);
    const float bottom_block_h = kBottomCount * kRailButtonHeight;
    const float target_y = ImGui::GetWindowHeight()
                         - bottom_block_h
                         - theme::space::sm;  // matches top padding
    if (target_y > ImGui::GetCursorPosY()) {
        ImGui::SetCursorPosY(target_y);
    }
    for (const TabDesc& t : kBottomTabs) {
        const bool active = (current_tab_ == t.id);
        if (theme::rail_button(t.label, t.icon, t.label, active)) {
            current_tab_ = t.id;
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
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

// ── Placeholder tab bodies ─────────────────────────────────────
// Each of these becomes its own .cpp file as the port progresses:
//   Activity  → screens/activity.cpp     (alone-state + workspace cards)
//   Layout    → screens/layout.cpp       (2-row drag/drop canvas, task #65)
//   Settings  → screens/settings.cpp     (shortcuts, pairing codes)
//   Access    → screens/access.cpp       (sign-in / mesh auth, task follow-up)
//   Help      → screens/help.cpp         (small)

void Shell::render_activity() {
    activity::render(orch_);
}

void Shell::render_layout() {
    layout::render(orch_);
}

void Shell::render_settings() {
    placeholder::render(
        "Settings",
        "Local autostart, network interface selection, port, log "
        "folder, and diagnostics will live here. Every change is "
        "persisted to the same user config the Python shell uses.");
}

void Shell::render_access() {
    placeholder::render(
        "Access",
        "Sign in once on any machine in the mesh; other machines "
        "on your LAN are activated automatically. No sign-ups, no "
        "account recovery flow — the mesh is scoped to your own "
        "desk.");
}

void Shell::render_help() {
    placeholder::render(
        "Help",
        "Guides, keyboard shortcuts, and troubleshooting tips will "
        "live here.");
}

}  // namespace unio_ui::screens
