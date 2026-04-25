/// @file activity.cpp
/// @brief Activity tab: welcome hero + peer list.

#include "ui/activity.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstring>

namespace unio_ui::ui::activity {

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

/// @brief Render the three-colour "Welcome to unIO" heading.
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

/// @brief Word-wrap @p text to @p wrap_width and centre each line.
/// Explicit `\n` characters break paragraphs; blank lines preserve
/// vertical rhythm.
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

void render_subtitle() {
    constexpr float kWrapWidth = 520.0f;
    const char* body =
        "unIO transforms the way you use multiple computers.\n\n\n"
        "Launch unIO on another computer — they'll find each "
        "other automatically.";
    render_wrapped_centered(body, kWrapWidth, theme::palette::paper_muted);
}

/// @brief Pick the status line based on the current auth state.
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

/// @brief Alone-state: welcome hero with vertical bias.
void render_alone_state(orchestrator::IOrchestrator& orch) {
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

/// @brief Running state: one status row per known peer.
void render_running_state(orchestrator::IOrchestrator& orch) {
    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text, "Your mesh");
    ImGui::PopFont();
    hairline();
    ImGui::Spacing();

    for (const auto& p : orch.peers()) {
        status_dot(p.online ? DotState::Ok : DotState::Idle);
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
    bool any_remote = false;
    for (const auto& p : orch.peers()) {
        if (!p.is_local) { any_remote = true; break; }
    }

    if (any_remote && orch.auth_state() == orchestrator::AuthState::SignedIn) {
        render_running_state(orch);
    } else {
        render_alone_state(orch);
    }
}

}  // namespace unio_ui::ui::activity
