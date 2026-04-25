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
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstring>
#include <string>
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

CardActions render_card(const orchestrator::Workspace& ws) {
    CardActions actions;

    const ImVec2 avail   = ImGui::GetContentRegionAvail();
    const ImVec2 origin  = ImGui::GetCursorScreenPos();
    const float  card_w  = avail.x;
    const float  inner_h = (ImGui::GetTextLineHeight()
                            + theme::space::sm) * 2.0f
                          + 2.0f * kCardPadY;

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

    // Subtitle row: member list as a comma-joined string, falls
    // back to a muted hint when the workspace has no members.
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x + kCardPadX,
               origin.y + kCardPadY
               + ImGui::GetTextLineHeight() + theme::space::sm));
    ImGui::PushFont(theme::font::body_sm);
    if (ws.members.empty()) {
        ImGui::TextColored(theme::palette::paper_faint,
                           "No computers yet. Click Edit to add some.");
    } else {
        std::vector<std::string> sorted(ws.members.begin(), ws.members.end());
        std::sort(sorted.begin(), sorted.end());
        std::string joined;
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (i != 0) joined += ", ";
            joined += sorted[i];
        }
        ImGui::TextColored(theme::palette::paper_muted, "%s", joined.c_str());
    }
    ImGui::PopFont();

    // Reserve the row in ImGui's layout cursor so subsequent cards
    // stack underneath this one with the right spacing.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + inner_h));
    ImGui::Dummy(ImVec2(card_w, theme::space::sm));
    return actions;
}

// ── Section: inline form ───────────────────────────────────────

/// @brief Render the Create / Edit form. Returns true once the form
/// closed (Save / Cancel / Delete) so the caller resets its mode.
bool render_form(orchestrator::IOrchestrator& orch,
                 std::optional<alert::Banner>& banner,
                 ViewState& v) {
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
            banner = alert::Banner{alert::Level::Info,
                                   "Workspace updated."};
        } else {
            orch.create_workspace(trimmed_name, v.form_members);
            banner = alert::Banner{alert::Level::Info,
                                   "Workspace created."};
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
            banner = alert::Banner{alert::Level::Warn,
                                   "Workspace deleted."};
            v.reset();
            return true;
        }
    }
    return false;
}

}  // namespace

void render(orchestrator::IOrchestrator& orch,
            std::optional<alert::Banner>& banner,
            ViewState& v) {
    if (v.mode == Mode::Create || v.mode == Mode::Edit) {
        // Form mode — replace the cards section entirely.
        render_form(orch, banner, v);
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

    std::string pending_edit;
    std::string pending_delete;
    for (const auto& ws : items) {
        const auto actions = render_card(ws);
        if (actions.edit_clicked)   pending_edit   = ws.id;
        if (actions.delete_clicked) pending_delete = ws.id;
    }

    if (!pending_edit.empty()) {
        if (auto ws = orch.workspace(pending_edit); ws) {
            start_edit(*ws, v);
        }
    } else if (!pending_delete.empty()) {
        // Single-click delete — no separate confirm yet. The alert
        // banner gives the user a visual receipt; an Undo is a
        // future ergonomics PR.
        orch.delete_workspace(pending_delete);
        banner = alert::Banner{alert::Level::Warn,
                               "Workspace deleted."};
    }
}

}  // namespace unio_ui::ui::workspaces
