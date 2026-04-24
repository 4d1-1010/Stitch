/*! @file activity.cpp
 *  @brief Activity-tab body — alone-state welcome + running list.
 */

#include "activity.hpp"

#include "imgui.h"

#include "../orchestrator.hpp"
#include "../theme.hpp"

#include <algorithm>
#include <cstdio>

namespace unio_ui::screens::activity {

namespace {

/// Horizontally centre the cursor so a widget of @p width renders
/// centred in the remaining content region.
void center_cursor_x(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float pad = (avail - width) * 0.5f;
    if (pad > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    }
}

/// "Welcome to unIO" at title size with the three-colour split
/// from `_activity_alone_state` (PAPER_TEXT / LILAC / PAPER_MUTED).
void render_title() {
    const char* prefix = "Welcome to ";
    const char* un     = "un";
    const char* io     = "IO";

    ImGui::PushFont(theme::font::title);

    const float total = ImGui::CalcTextSize(prefix).x
                      + ImGui::CalcTextSize(un).x
                      + ImGui::CalcTextSize(io).x;
    center_cursor_x(total);

    ImGui::TextColored(theme::palette::paper_text,  "%s", prefix);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(theme::palette::lilac,       "%s", un);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(theme::palette::paper_muted, "%s", io);

    ImGui::PopFont();
}

void render_subtitle() {
    constexpr float kWrapWidth = 520.0f;
    const char* body =
        "unIO transforms the way you use multiple computers.\n\n\n"
        "Launch unIO on another computer — they'll find each "
        "other automatically.";

    center_cursor_x(kWrapWidth);
    const float start_x = ImGui::GetCursorPosX();
    ImGui::PushTextWrapPos(start_x + kWrapWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::paper_muted);
    ImGui::TextUnformatted(body);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

/// Tri-state status line below the subtitle — exactly mirrors
/// shell.py's _activity_alone_state dispatch.
void render_status(orchestrator::IOrchestrator& orch) {
    const char* status = nullptr;
    switch (orch.auth_state()) {
        case orchestrator::AuthState::GracePeriod:
            status = "Looking for an activated unIO on your LAN…";
            break;
        case orchestrator::AuthState::SignedOut:
            status = "Sign in (Access tab) on this or any other PC "
                     "to activate the mesh.";
            break;
        case orchestrator::AuthState::SignedIn:
            status = "Searching for peers on your LAN…";
            break;
    }

    ImGui::PushFont(theme::font::body_sm);
    const float w = ImGui::CalcTextSize(status).x;
    center_cursor_x(w);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::paper_muted);
    ImGui::TextUnformatted(status);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void render_alone_state(orchestrator::IOrchestrator& orch) {
    // Vertically bias the welcome block so it sits slightly
    // above mathematical centre — reads as the hero content of
    // an otherwise empty page. Scales with window height rather
    // than pinning a fixed offset like the Python did (which
    // assumed a top-bar above us; we removed the top bar, so a
    // fixed offset would read as "too high").
    //
    // Estimated block height covers title + spacings + 3-line
    // subtitle + status. Tweak if we change the copy.
    constexpr float kBlockHeight = 210.0f;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float top = std::max(theme::space::xl,
                               (avail_h - kBlockHeight) * 0.35f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + top);

    render_title();
    ImGui::Spacing();
    ImGui::Spacing();
    render_subtitle();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();
    render_status(orch);
}

void render_running_state(orchestrator::IOrchestrator& orch) {
    // First cut: just a peer-list. Workspace cards (the Python's
    // real running-state) plug in later behind the orchestrator
    // interface once CRDT snapshots are there.
    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text, "Your mesh");
    ImGui::PopFont();
    theme::hairline();
    ImGui::Spacing();

    const auto peers = orch.peers();
    for (const auto& p : peers) {
        theme::status_dot(p.online ? theme::DotState::Ok
                                   : theme::DotState::Idle);
        ImGui::SameLine();
        ImGui::TextColored(theme::palette::paper_text,
                           "%s", p.display_name.c_str());
        ImGui::SameLine();
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_muted,
                           "· %s%s",
                           p.address.c_str(),
                           p.is_local ? " · this PC" : "");
        ImGui::PopFont();
    }
}

}  // namespace

void render(orchestrator::IOrchestrator& orch) {
    // "Alone" while we have no remote peers; "running" once any
    // remote peer (is_local=false) shows up.
    const auto peers = orch.peers();
    bool any_remote = false;
    for (const auto& p : peers) {
        if (!p.is_local) { any_remote = true; break; }
    }

    if (any_remote && orch.auth_state() == orchestrator::AuthState::SignedIn) {
        render_running_state(orch);
    } else {
        render_alone_state(orch);
    }
}

}  // namespace unio_ui::screens::activity
