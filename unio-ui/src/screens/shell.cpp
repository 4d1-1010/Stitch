/*! @file shell.cpp
 *  @brief Main-window layout implementation.
 */

#include "shell.hpp"

#include "imgui.h"

#include "../orchestrator.hpp"
#include "../theme.hpp"
#include "activity.hpp"
#include "layout.hpp"

#include <cstdio>

namespace unio_ui::screens {

namespace {

constexpr float kRailWidth = 108.0f;
constexpr float kTopBarHeight = 56.0f;

struct TabDesc {
    Shell::Tab id;
    const char* label;
    const char* glyph_utf8;  // UTF-8 emoji/symbol
};

// Kept in sync with shell.py's self._tabs ordering.
constexpr TabDesc kTabs[] = {
    {Shell::Tab::Activity, "Activity", "\xE2\x8F\xB5"},  // ⏵
    {Shell::Tab::Layout,   "Layout",   "\xE2\x96\xA6"},  // ▦
    {Shell::Tab::Settings, "Settings", "\xE2\x9A\x99"},  // ⚙
    {Shell::Tab::Access,   "Access",   "\xE2\x8A\x95"},  // ⊕
    {Shell::Tab::Help,     "Help",     "\x3F"},          // ?
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

    for (const TabDesc& t : kTabs) {
        const bool active = (current_tab_ == t.id);
        if (theme::rail_button(t.label, t.glyph_utf8, t.label, active)) {
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
    ImGui::TextUnformatted("Settings");
    theme::hairline();
    ImGui::TextColored(theme::palette::paper_muted,
                       "Shortcuts, pairing, log verbosity — port pending.");
}

void Shell::render_access() {
    ImGui::TextUnformatted("Access");
    theme::hairline();
    ImGui::TextColored(theme::palette::paper_muted,
                       "Sign-in + mesh auth — port pending.");
}

void Shell::render_help() {
    ImGui::TextUnformatted("Help");
    theme::hairline();
    ImGui::TextColored(theme::palette::paper_muted,
                       "Shortcuts cheat-sheet — port pending.");
}

}  // namespace unio_ui::screens
