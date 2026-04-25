/// @file activity.cpp
/// @brief Activity tab: welcome hero + peer list.

#include "ui/activity.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/activity_workspaces.hpp"
#include "ui/machine_color.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

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

/// @brief "Unify · N computers" header + machine_id subtitle.
void render_activity_header(orchestrator::IOrchestrator& orch) {
    const std::size_t pc_count = orch.peers().size();
    const std::string title = "Unify · " + std::to_string(pc_count)
                              + " computer" + (pc_count == 1 ? "" : "s");

    ImGui::PushFont(theme::font::title);
    ImGui::TextColored(theme::palette::paper_text, "%s", title.c_str());
    ImGui::PopFont();

    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_muted,
                       "%s", orch.local_machine_id().c_str());
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Spacing();
}

/// @brief Render one peer row: machine-coloured circle + name +
/// muted IP / "this PC" suffix. Online state attenuates the dot
/// alpha so paired-but-offline reads as a faded chip.
void render_peer_row(const orchestrator::Peer& p) {
    constexpr float kDot = 12.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec4 col = machine_color(p.machine_id);
    if (!p.online) col.w = 0.4f;
    dl->AddCircleFilled(
        ImVec2(c0.x + kDot * 0.5f, c0.y + ImGui::GetTextLineHeight() * 0.5f),
        kDot * 0.5f,
        ImGui::ColorConvertFloat4ToU32(col),
        20);
    ImGui::Dummy(ImVec2(kDot + theme::space::sm, ImGui::GetTextLineHeight()));
    ImGui::SameLine();

    ImGui::TextColored(theme::palette::paper_text,
                       "%s", p.display_name.c_str());
    ImGui::SameLine();

    ImGui::PushFont(theme::font::body_sm);
    if (p.is_local) {
        ImGui::TextColored(theme::palette::paper_muted, "· this PC");
    } else if (!p.address.empty()) {
        ImGui::TextColored(theme::palette::paper_muted,
                           "· %s", p.address.c_str());
    }
    ImGui::PopFont();
}

/// @brief Section: titled list of peer rows. Renders an italic
/// muted line when @p machine_ids is empty + @p empty_text is set.
void render_pc_section(const char* title,
                       const std::vector<orchestrator::Peer>& peers,
                       const char* empty_text) {
    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text, "%s", title);
    ImGui::PopFont();
    hairline();
    ImGui::Spacing();

    if (peers.empty()) {
        if (empty_text != nullptr) {
            ImGui::PushFont(theme::font::body_sm);
            ImGui::TextColored(theme::palette::paper_faint,
                               "%s", empty_text);
            ImGui::PopFont();
        }
        ImGui::Spacing();
        ImGui::Spacing();
        return;
    }

    for (const auto& p : peers) render_peer_row(p);
    ImGui::Spacing();
    ImGui::Spacing();
}

/// @brief Running state: header + (Connected | Unassigned) PC
/// section + workspaces section. The view splits the PC list
/// based on workspace membership: when at least one workspace
/// exists, only PCs that don't belong to any workspace appear in
/// the standalone section (the assigned ones render inside their
/// workspace card). Action receipts surface through the global
/// bottom status bar; the workspaces view-mode survives across
/// frames as a static-local.
void render_running_state(orchestrator::IOrchestrator& orch) {
    static workspaces::ViewState workspaces_view;

    render_activity_header(orch);

    const auto all_peers = orch.peers();
    const auto workspaces_list = orch.workspaces();
    const auto assignments = orch.pc_workspace_assignments();

    std::unordered_set<std::string> assigned_ids;
    for (const auto& [mid, _] : assignments) assigned_ids.insert(mid);

    if (workspaces_list.empty()) {
        render_pc_section("Connected computers", all_peers,
                          "Waiting for computers to connect…");
    } else {
        std::vector<orchestrator::Peer> unassigned;
        unassigned.reserve(all_peers.size());
        for (const auto& p : all_peers) {
            if (assigned_ids.count(p.machine_id) == 0) {
                unassigned.push_back(p);
            }
        }
        render_pc_section("Unassigned", unassigned,
                          "Every computer belongs to a workspace.");
    }

    workspaces::render(orch, workspaces_view);
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
