/// @file activity_workspaces.cpp
/// @brief Activity-tab workspaces section.
///
/// Three view modes — List, Create, Edit — dispatched from a
/// caller-owned @ref ViewState. Cards + form share theme and
/// helper utilities scoped to this TU.

#include "ui/activity_workspaces.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/machine_color.hpp"
#include "ui/primitives.hpp"
#include "ui/status_bar.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace unio_ui::ui::workspaces {

namespace {

// ── Tunable constants ───────────────────────────────────────────

/// @brief Minimum number of mesh PCs required before "+ Create
/// workspace" is offered. Below this, the section shows a hint
/// instead of a disabled button.
constexpr std::size_t kMinPcsForWorkspace = 2;

/// @brief Card body padding.
constexpr float       kCardPadX = 18.0f;
constexpr float       kCardPadY = 12.0f;

// ── Helpers shared by the section ──────────────────────────────

/// @brief Count mesh PCs (local + remote). Workspaces care about
/// total cardinality, not paired/online status.
std::size_t pc_count(orchestrator::IOrchestrator& orch) {
    return orch.peers().size();
}

/// @brief Set form_members + name_buffer from an existing workspace
/// so the Edit form opens populated.
void seed_form_from(const orchestrator::Workspace& ws, ViewState& v) {
    v.name_buffer.fill(0);
    const std::size_t copy =
        std::min(ws.name.size(), v.name_buffer.size() - 1);
    std::memcpy(v.name_buffer.data(), ws.name.data(), copy);
    v.form_members = ws.members;
}

/// @brief Open the Create form with a default name + no members.
void start_create(ViewState& v) {
    v.reset();
    v.mode = Mode::Create;
    constexpr const char* kDefaultName = "Workspace";
    std::memcpy(v.name_buffer.data(), kDefaultName, std::strlen(kDefaultName));
}

/// @brief Open the Edit form populated from @p ws.
void start_edit(const orchestrator::Workspace& ws, ViewState& v) {
    v.reset();
    v.mode       = Mode::Edit;
    v.editing_id = ws.id;
    seed_form_from(ws, v);
}

// ── Section: header ────────────────────────────────────────────

/// @brief Render the "Workspaces" title row + "+ Create" button.
/// Returns true when the user clicked Create.
bool render_section_header(orchestrator::IOrchestrator& orch,
                           bool offer_create_button) {
    bool clicked_create = false;

    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text, "Workspaces");
    ImGui::PopFont();

    if (offer_create_button && pc_count(orch) >= kMinPcsForWorkspace) {
        ImGui::SameLine();
        const float right_pad =
            ImGui::CalcTextSize("+ Create workspace").x + 40.0f;
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX()
            + ImGui::GetContentRegionAvail().x - right_pad);
        if (pill_button("+ Create workspace", PillVariant::Secondary)) {
            clicked_create = true;
        }
    }

    hairline();
    ImGui::Spacing();
    return clicked_create;
}

// ── Section: empty-state prompts ───────────────────────────────

/// @brief Below-PCs prompt shown when no workspaces exist yet.
/// Returns true if the user wants to start creating.
bool render_empty_prompt(orchestrator::IOrchestrator& orch) {
    if (pc_count(orch) < kMinPcsForWorkspace) {
        ImGui::PushFont(theme::font::body);
        ImGui::TextColored(
            theme::palette::paper_muted,
            "A workspace needs at least 2 computers. "
            "Launch unIO on another computer to continue.");
        ImGui::PopFont();
        return false;
    }

    ImGui::PushFont(theme::font::body);
    ImGui::TextColored(
        theme::palette::paper_muted,
        "Group computers together to share a cursor, keyboard, "
        "and clipboard between them.");
    ImGui::PopFont();
    ImGui::Spacing();
    return pill_button("+ Create workspace", PillVariant::Primary);
}

// ── Section: workspace card ────────────────────────────────────

/// @brief Render one workspace card. Returns:
///   `edit_clicked`   — user wants to edit
///   `delete_clicked` — user wants to delete
struct CardActions {
    bool edit_clicked   = false;
    bool delete_clicked = false;
};

/// @brief Render one peer-tile row inside a workspace card body.
/// Mirrors the Activity body's row look so a member of a workspace
/// reads identically to a peer in the standalone "Connected" list.
void render_member_row(const std::string& machine_id,
                       const std::string& display_name,
                       bool online) {
    constexpr float kDot = 12.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec4 col = machine_color(machine_id);
    if (!online) col.w = 0.4f;
    dl->AddCircleFilled(
        ImVec2(c0.x + kDot * 0.5f, c0.y + ImGui::GetTextLineHeight() * 0.5f),
        kDot * 0.5f,
        ImGui::ColorConvertFloat4ToU32(col),
        20);
    ImGui::Dummy(ImVec2(kDot + theme::space::sm, ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::TextColored(theme::palette::paper_text,
                       "%s", display_name.c_str());
}

CardActions render_card(const orchestrator::Workspace& ws,
                        const std::unordered_map<std::string,
                                                 orchestrator::Peer>& peer_index) {
    CardActions actions;

    // Card height now scales with member count: header row + one
    // line per member (or the empty-state line). Computed up front
    // so the rect background fills underneath every row.
    const float line_h = ImGui::GetTextLineHeight();
    const std::size_t row_count =
        ws.members.empty() ? 1u : ws.members.size();
    const float inner_h =
        line_h + theme::space::sm                 // title row
      + theme::space::sm                          // gap
      + static_cast<float>(row_count) * (line_h + 4.0f)
      + 2.0f * kCardPadY;

    const ImVec2 avail   = ImGui::GetContentRegionAvail();
    const ImVec2 origin  = ImGui::GetCursorScreenPos();
    const float  card_w  = avail.x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin,
                      ImVec2(origin.x + card_w, origin.y + inner_h),
                      ImGui::ColorConvertFloat4ToU32(theme::palette::paper_surface),
                      theme::radius::md);

    ImGui::SetCursorScreenPos(ImVec2(origin.x + kCardPadX,
                                     origin.y + kCardPadY));

    // Title row: name + Edit/Delete on the right.
    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text,
                       "%s", ws.name.empty() ? "Workspace" : ws.name.c_str());
    ImGui::PopFont();

    // Buttons aligned to the right edge of the card.
    const float btn_block_w =
        ImGui::CalcTextSize("Edit").x   + 28.0f
      + ImGui::CalcTextSize("Delete").x + 28.0f
      + theme::space::sm;
    ImGui::SameLine();
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX()
        + ImGui::GetContentRegionAvail().x - btn_block_w
        - kCardPadX);
    if (pill_button((std::string("Edit##") + ws.id).c_str(),
                     PillVariant::Secondary)) {
        actions.edit_clicked = true;
    }
    ImGui::SameLine();
    if (pill_button((std::string("Delete##") + ws.id).c_str(),
                     PillVariant::Ghost)) {
        actions.delete_clicked = true;
    }

    // Member rows.
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x + kCardPadX,
               origin.y + kCardPadY
               + ImGui::GetTextLineHeight() + theme::space::sm));
    if (ws.members.empty()) {
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_faint,
                           "No computers yet. Click Edit to add some.");
        ImGui::PopFont();
    } else {
        std::vector<std::string> sorted(ws.members.begin(), ws.members.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& mid : sorted) {
            std::string display_name = mid;
            bool        online       = false;
            if (auto it = peer_index.find(mid); it != peer_index.end()) {
                display_name = it->second.display_name.empty()
                               ? it->second.machine_id
                               : it->second.display_name;
                online       = it->second.online;
            }
            ImGui::SetCursorScreenPos(
                ImVec2(origin.x + kCardPadX,
                       ImGui::GetCursorScreenPos().y));
            render_member_row(mid, display_name, online);
        }
    }

    // Reserve the row in ImGui's layout cursor so subsequent cards
    // stack underneath this one with the right spacing.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + inner_h));
    ImGui::Dummy(ImVec2(card_w, theme::space::sm));
    return actions;
}

// ── Section: inline form ───────────────────────────────────────

/// @brief Render the Create / Edit form. Returns true once the form
/// closed (Save / Cancel / Delete) so the caller resets its mode.
bool render_form(orchestrator::IOrchestrator& orch, ViewState& v) {
    const bool editing = (v.mode == Mode::Edit);

    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text,
                       editing ? "Edit workspace" : "New workspace");
    ImGui::PopFont();
    hairline();
    ImGui::Spacing();

    // Name input.
    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_muted, "Name");
    ImGui::PopFont();
    ImGui::PushItemWidth(360.0f);
    ImGui::InputText("##ws-name",
                     v.name_buffer.data(), v.name_buffer.size());
    ImGui::PopItemWidth();
    ImGui::Spacing();

    // Member checkboxes — one per known PC.
    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_muted, "Computers");
    ImGui::PopFont();
    ImGui::Spacing();

    auto peers = orch.peers();
    std::sort(peers.begin(), peers.end(),
              [](const orchestrator::Peer& a, const orchestrator::Peer& b) {
                  return a.machine_id < b.machine_id;
              });
    for (const auto& p : peers) {
        const std::string label = p.display_name.empty()
                                  ? p.machine_id : p.display_name;
        bool selected = v.form_members.count(p.machine_id) > 0;
        if (ImGui::Checkbox((label + "##ws-mem-" + p.machine_id).c_str(),
                            &selected)) {
            if (selected) v.form_members.insert(p.machine_id);
            else          v.form_members.erase(p.machine_id);
        }
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Action row: Save · Cancel · (Delete if editing).
    const std::string trimmed_name(v.name_buffer.data());
    const bool can_save =
        !trimmed_name.empty() && trimmed_name.find_first_not_of(' ') != std::string::npos;

    if (!can_save) ImGui::BeginDisabled();
    if (pill_button("Save##ws-form", PillVariant::Primary)) {
        if (editing) {
            orch.rename_workspace(v.editing_id, trimmed_name);
            orch.set_workspace_members(v.editing_id, v.form_members);
            status::post(status::Level::Info, "Workspace updated.");
        } else {
            orch.create_workspace(trimmed_name, v.form_members);
            status::post(status::Level::Info, "Workspace created.");
        }
        v.reset();
        if (!can_save) ImGui::EndDisabled();
        return true;
    }
    if (!can_save) ImGui::EndDisabled();

    ImGui::SameLine();
    if (pill_button("Cancel##ws-form", PillVariant::Secondary)) {
        v.reset();
        return true;
    }

    if (editing) {
        ImGui::SameLine();
        if (pill_button("Delete##ws-form", PillVariant::Danger)) {
            orch.delete_workspace(v.editing_id);
            status::post(status::Level::Warn, "Workspace deleted.");
            v.reset();
            return true;
        }
    }
    return false;
}

}  // namespace

void render(orchestrator::IOrchestrator& orch, ViewState& v) {
    if (v.mode == Mode::Create || v.mode == Mode::Edit) {
        // Form mode — replace the cards section entirely.
        render_form(orch, v);
        return;
    }

    const auto items = orch.workspaces();

    if (items.empty()) {
        // No workspaces yet — single header + prompt below the
        // peer list.
        if (render_section_header(orch, /*offer_create_button=*/false)) {
            // Header doesn't offer Create on the empty path; the
            // empty prompt does.
        }
        if (render_empty_prompt(orch)) {
            start_create(v);
        }
        return;
    }

    // Cards section.
    if (render_section_header(orch, /*offer_create_button=*/true)) {
        start_create(v);
        return;
    }

    // Build a {machine_id → Peer} index once per frame so each
    // card can resolve its members to display names + online state
    // without re-scanning the peer list per row.
    std::unordered_map<std::string, orchestrator::Peer> peer_index;
    for (const auto& p : orch.peers()) peer_index.emplace(p.machine_id, p);

    std::string pending_edit;
    std::string pending_delete;
    for (const auto& ws : items) {
        const auto actions = render_card(ws, peer_index);
        if (actions.edit_clicked)   pending_edit   = ws.id;
        if (actions.delete_clicked) pending_delete = ws.id;
    }

    if (!pending_edit.empty()) {
        if (auto ws = orch.workspace(pending_edit); ws) {
            start_edit(*ws, v);
        }
    } else if (!pending_delete.empty()) {
        // Single-click delete — no separate confirm yet. The status
        // bar gives the user a visual receipt; an Undo is a future
        // ergonomics PR.
        orch.delete_workspace(pending_delete);
        status::post(status::Level::Warn, "Workspace deleted.");
    }
}

}  // namespace unio_ui::ui::workspaces
