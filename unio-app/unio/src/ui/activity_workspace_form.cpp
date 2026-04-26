/// @file activity_workspace_form.cpp
/// @brief Inline Create / Edit form for the workspaces section.

#include "ui/activity_workspace_form.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace unio_ui::ui::workspaces {

namespace {

/// @brief Set form_members + name_buffer from an existing workspace
/// so the Edit form opens populated.
void seed_form_from(const orchestrator::Workspace& ws, ViewState& v) {
    v.name_buffer.fill(0);
    const std::size_t copy =
        std::min(ws.name.size(), v.name_buffer.size() - 1);
    std::memcpy(v.name_buffer.data(), ws.name.data(), copy);
    v.form_members = ws.members;
}

}  // namespace

void start_create(ViewState& v) {
    v.reset();
    v.mode = Mode::Create;
    constexpr const char* kDefaultName = "Workspace";
    std::memcpy(v.name_buffer.data(), kDefaultName, std::strlen(kDefaultName));
}

void start_edit(const orchestrator::Workspace& ws, ViewState& v) {
    v.reset();
    v.mode       = Mode::Edit;
    v.editing_id = ws.id;
    seed_form_from(ws, v);
}

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
        } else {
            orch.create_workspace(trimmed_name, v.form_members);
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
            v.reset();
            return true;
        }
    }
    return false;
}

}  // namespace unio_ui::ui::workspaces
