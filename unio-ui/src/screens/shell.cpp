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

    // Layout matches shell.py's _build:
    //   +----+-----------------------+
    //   |rail| top bar (logo centre) |
    //   |    +-----------------------+
    //   |    | content               |
    //   +----+-----------------------+
    // Rail spans full height. Top bar sits above content only
    // (NOT over the rail) so the logo's horizontal centre lands
    // in the middle of the content column.
    render_rail();
    ImGui::SameLine(0.0f, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::paper_bg);
    ImGui::BeginChild("##right-col", ImVec2(0, 0),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    render_top_bar();
    render_content();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    ImGui::End();
    ImGui::PopStyleVar(3);
}

void Shell::render_top_bar() {
    // Matches shell.py _build_top_bar: PAPER_BG (blends into the
    // content page), fixed 64-px height, logo centered both
    // axes. Status lives in the top-right corner — small, so the
    // logo still reads as the bar's centre of gravity.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::palette::paper_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##topbar", ImVec2(0.0f, kTopBarHeight),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    const ImVec2 bar_p = ImGui::GetCursorScreenPos();
    const float bar_w = ImGui::GetContentRegionAvail().x;
    const float bar_h = kTopBarHeight;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ── Centred logo ───────────────────────────────────────────
    const auto& logo = theme::logo_texture();
    if (logo.texture) {
        // Slightly smaller than the raw 48×48 so breathing room
        // around the mark matches the 64-px bar.
        constexpr float kLogoSize = 40.0f;
        const ImVec2 tl(bar_p.x + (bar_w - kLogoSize) * 0.5f,
                        bar_p.y + (bar_h - kLogoSize) * 0.5f);
        dl->AddImage(logo.texture, tl,
                     ImVec2(tl.x + kLogoSize, tl.y + kLogoSize));
    } else {
        // Text fallback while the texture hasn't been uploaded
        // (shouldn't happen in normal runs — platform does it at
        // init — but keeps the bar non-empty if upload fails).
        ImGui::PushFont(theme::font::title);
        const char* a = "un";
        const char* b = "IO";
        const float wa = ImGui::CalcTextSize(a).x;
        const float wb = ImGui::CalcTextSize(b).x;
        const float total = wa + wb;
        const ImVec2 tp(bar_p.x + (bar_w - total) * 0.5f,
                        bar_p.y + (bar_h - ImGui::GetTextLineHeight()) * 0.5f);
        dl->AddText(tp,
                    ImGui::ColorConvertFloat4ToU32(theme::palette::lilac), a);
        dl->AddText(ImVec2(tp.x + wa, tp.y),
                    ImGui::ColorConvertFloat4ToU32(theme::palette::paper_text), b);
        ImGui::PopFont();
    }

    // ── Top-right status: dot + "<state> · <hostname>" ─────────
    const auto astate = orch_.auth_state();
    const char* state_label =
        (astate == orchestrator::AuthState::SignedIn)    ? "online"    :
        (astate == orchestrator::AuthState::GracePeriod) ? "searching" :
                                                           "offline";
    const theme::DotState dot_state =
        (astate == orchestrator::AuthState::SignedIn)    ? theme::DotState::Ok   :
        (astate == orchestrator::AuthState::GracePeriod) ? theme::DotState::Warn :
                                                           theme::DotState::Bad;

    char right_label[128];
    std::snprintf(right_label, sizeof(right_label), "%s · %s",
                  state_label, orch_.local_display_name().c_str());

    // Measure with body_sm so the right-edge inset is right.
    ImGui::PushFont(theme::font::body_sm);
    const ImVec2 tsz = ImGui::CalcTextSize(right_label);
    ImGui::PopFont();

    constexpr float kRightPad = 16.0f;
    constexpr float kDotDiam  = 8.0f;
    const float right_edge = bar_p.x + bar_w - kRightPad;
    const float text_x = right_edge - tsz.x;
    const float dot_x  = text_x - kDotDiam - 6.0f;
    const float mid_y  = bar_p.y + bar_h * 0.5f;

    // Dot.
    dl->AddCircleFilled(
        ImVec2(dot_x + kDotDiam * 0.5f, mid_y),
        kDotDiam * 0.5f,
        ImGui::ColorConvertFloat4ToU32(
            dot_state == theme::DotState::Ok   ? theme::palette::mint  :
            dot_state == theme::DotState::Warn ? theme::palette::amber :
            dot_state == theme::DotState::Bad  ? theme::palette::coral :
                                                 theme::palette::paper_faint),
        12);
    // Label.
    dl->AddText(theme::font::body_sm, theme::font::body_sm->LegacySize,
                ImVec2(text_x, mid_y - tsz.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(theme::palette::paper_muted),
                right_label);

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
