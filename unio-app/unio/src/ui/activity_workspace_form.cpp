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
#include <unordered_set>

namespace unio_ui::ui::workspaces {

namespace {

/// @brief Seed name_buffer + per-cap member sets + every settings
/// field from an existing workspace so the Edit form opens
/// populated.
void seed_form_from(const orchestrator::Workspace& ws, ViewState& v) {
    v.name_buffer.fill(0);
    const std::size_t copy =
        std::min(ws.name.size(), v.name_buffer.size() - 1);
    std::memcpy(v.name_buffer.data(), ws.name.data(), copy);
    v.form_members             = ws.members;
    v.form_input_members       = ws.input_members;
    v.form_clipboard_members   = ws.clipboard_members;
    v.clipboard_max            = static_cast<int>(ws.clipboard_max);
    v.clipboard_rich           = ws.clipboard_rich;
    v.clipboard_files          = ws.clipboard_files;
    v.cursor_edge_margin       = ws.cursor_edge_margin;
    v.cursor_require_modifier  = ws.cursor_require_modifier;
    v.cursor_block_hotkeys     = ws.cursor_block_hotkeys;
    v.auto_unlock              = static_cast<int>(ws.auto_unlock);

    // Snapshot the seeded values so the Save button can light
    // up only after the user actually changes something.
    v.baseline_name                    = ws.name;
    v.baseline_members                 = ws.members;
    v.baseline_input_members           = ws.input_members;
    v.baseline_clipboard_members       = ws.clipboard_members;
    v.baseline_clipboard_max           = v.clipboard_max;
    v.baseline_clipboard_rich          = v.clipboard_rich;
    v.baseline_clipboard_files         = v.clipboard_files;
    v.baseline_cursor_edge_margin      = v.cursor_edge_margin;
    v.baseline_cursor_require_modifier = v.cursor_require_modifier;
    v.baseline_cursor_block_hotkeys    = v.cursor_block_hotkeys;
    v.baseline_auto_unlock             = v.auto_unlock;
}

/// @brief True when any field on the form differs from the
/// baseline captured at seed time. Drives the Save button's
/// dirty gate.
bool form_dirty(const ViewState& v) {
    if (v.baseline_name != std::string(v.name_buffer.data())) return true;
    if (v.baseline_members              != v.form_members) return true;
    if (v.baseline_input_members        != v.form_input_members) return true;
    if (v.baseline_clipboard_members    != v.form_clipboard_members) return true;
    if (v.baseline_clipboard_max        != v.clipboard_max) return true;
    if (v.baseline_clipboard_rich       != v.clipboard_rich) return true;
    if (v.baseline_clipboard_files      != v.clipboard_files) return true;
    if (v.baseline_cursor_edge_margin   != v.cursor_edge_margin) return true;
    if (v.baseline_cursor_require_modifier
        != v.cursor_require_modifier) return true;
    if (v.baseline_cursor_block_hotkeys != v.cursor_block_hotkeys) return true;
    if (v.baseline_auto_unlock          != v.auto_unlock) return true;
    return false;
}

/// @brief Pull the current form's settings into a struct ready
/// for orch.set_workspace_settings().
orchestrator::WorkspaceSettings settings_from_form(const ViewState& v) {
    orchestrator::WorkspaceSettings s;
    s.clipboard_max          = static_cast<orchestrator::ClipboardLimit>(
        std::clamp(v.clipboard_max, 0, 4));
    s.clipboard_rich         = v.clipboard_rich;
    s.clipboard_files        = v.clipboard_files;
    s.cursor_edge_margin     = std::clamp(v.cursor_edge_margin, 4, 50);
    s.cursor_require_modifier = v.cursor_require_modifier;
    s.cursor_block_hotkeys   = v.cursor_block_hotkeys;
    s.auto_unlock            = static_cast<orchestrator::AutoUnlock>(
        std::clamp(v.auto_unlock, 0, 3));
    return s;
}

// ── Form-section helpers ──────────────────────────────────────

void section_header(const char* text) {
    ImGui::Dummy(ImVec2(0.0f, theme::space::sm));
    ImGui::PushFont(theme::font::body);
    ImGui::TextColored(theme::palette::paper_text, "%s", text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
}

void labeled_combo(const char* id, const char* label,
                   int& selected, const char* const* items, int item_count) {
    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_text, "%s", label);
    ImGui::PopFont();
    ImGui::SameLine(220.0f);
    ImGui::SetNextItemWidth(180.0f);
    int idx = std::clamp(selected, 0, item_count - 1);
    if (ImGui::Combo(id, &idx, items, item_count)) {
        selected = idx;
    }
}

void labeled_int_input(const char* id, const char* label,
                       int& value, int min_value, int max_value) {
    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_text, "%s", label);
    ImGui::PopFont();
    ImGui::SameLine(220.0f);
    ImGui::SetNextItemWidth(80.0f);
    int v = value;
    if (ImGui::InputInt(id, &v, 0, 0)) {
        value = std::clamp(v, min_value, max_value);
    }
}

void wrapping_checkbox(const char* id, const char* label, bool& value) {
    ImGui::Checkbox(id, &value);
    ImGui::SameLine();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 360.0f);
    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_text, "%s", label);
    ImGui::PopFont();
    ImGui::PopTextWrapPos();
}

}  // namespace

void start_create(ViewState& v) {
    v.reset();
    v.mode = Mode::Create;
    constexpr const char* kDefaultName = "Workspace";
    std::memcpy(v.name_buffer.data(), kDefaultName, std::strlen(kDefaultName));
    // Empty member sets — user picks Input + Clipboard per PC.

    // Baseline for the dirty gate: the form's pristine starting
    // state. Save lights up the moment the user changes anything
    // (name, member, cap toggle, settings field).
    v.baseline_name                    = kDefaultName;
    v.baseline_members                 = v.form_members;
    v.baseline_input_members           = v.form_input_members;
    v.baseline_clipboard_members       = v.form_clipboard_members;
    v.baseline_clipboard_max           = v.clipboard_max;
    v.baseline_clipboard_rich          = v.clipboard_rich;
    v.baseline_clipboard_files         = v.clipboard_files;
    v.baseline_cursor_edge_margin      = v.cursor_edge_margin;
    v.baseline_cursor_require_modifier = v.cursor_require_modifier;
    v.baseline_cursor_block_hotkeys    = v.cursor_block_hotkeys;
    v.baseline_auto_unlock             = v.auto_unlock;
}

void start_edit(const orchestrator::Workspace& ws, ViewState& v) {
    v.reset();
    v.mode       = Mode::Edit;
    v.editing_id = ws.id;
    seed_form_from(ws, v);
}

bool render_form(orchestrator::IOrchestrator& orch, ViewState& v) {
    const bool editing = (v.mode == Mode::Edit);
    // The title + hairline are rendered by the manager host
    // (activity.cpp) so they stay pinned outside the scroll child.

    // Name input.
    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_muted, "Name");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, theme::space::xs));
    ImGui::PushItemWidth(360.0f);
    ImGui::InputText("##ws-name",
                     v.name_buffer.data(), v.name_buffer.size());
    ImGui::PopItemWidth();
    ImGui::Dummy(ImVec2(0.0f, theme::space::sm));

    // Member rows — one per known PC. Each row carries an Input
    // checkbox + a Clipboard checkbox. Input is the unified
    // mouse + keyboard toggle (separate Cursor / Keyboard
    // checkboxes proved redundant — the user always wanted both
    // or neither). A PC counts as a member when its Member box
    // is ticked. Rendered through ImGui::Table so the right-side
    // columns get fixed widths and never clip regardless of how
    // wide the workspace card is.
    auto peers = orch.peers();
    std::sort(peers.begin(), peers.end(),
              [](const orchestrator::Peer& a, const orchestrator::Peer& b) {
                  return a.machine_id < b.machine_id;
              });
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_SizingFixedFit
        | ImGuiTableFlags_NoBordersInBody
        | ImGuiTableFlags_PadOuterX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));
    if (ImGui::BeginTable("##ws-members", 3, kTableFlags)) {
        ImGui::TableSetupColumn("Computer",
                                 ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Input",
                                 ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Clipboard",
                                 ImGuiTableColumnFlags_WidthFixed, 78.0f);

        // Header row — only the right-side columns label themselves;
        // the Member checkbox sits in the Computer column to the
        // left of each PC name and needs no header (its position
        // is self-explanatory).
        const char* headers[] = {"Computer", "Input", "Clipboard"};
        ImGui::TableNextRow();
        for (int c = 0; c < 3; ++c) {
            ImGui::TableSetColumnIndex(c);
            ImGui::PushFont(theme::font::body_sm);
            ImGui::TextColored(theme::palette::paper_muted, "%s", headers[c]);
            ImGui::PopFont();
        }

        for (const auto& p : peers) {
            const std::string label = p.display_name.empty()
                                      ? p.machine_id : p.display_name;
            const bool is_member = v.form_members.count(p.machine_id) > 0;
            ImGui::TableNextRow();

            // Computer column: Member checkbox + PC name. Toggling
            // Member on defaults caps to "on" (most common intent);
            // toggling off drops every cap for that PC.
            ImGui::TableSetColumnIndex(0);
            bool member_on = is_member;
            if (ImGui::Checkbox(("##ws-mem-" + p.machine_id).c_str(),
                                &member_on)) {
                if (member_on) {
                    v.form_members.insert(p.machine_id);
                    v.form_input_members.insert(p.machine_id);
                    v.form_clipboard_members.insert(p.machine_id);
                } else {
                    v.form_members.erase(p.machine_id);
                    v.form_input_members.erase(p.machine_id);
                    v.form_clipboard_members.erase(p.machine_id);
                }
            }
            ImGui::SameLine(0.0f, theme::space::sm);
            ImGui::TextColored(theme::palette::paper_text,
                               "%s", label.c_str());

            // Input / Clipboard — only togglable when Member is on.
            if (!is_member) ImGui::BeginDisabled();
            ImGui::TableSetColumnIndex(1);
            bool in_on = v.form_input_members.count(p.machine_id) > 0;
            if (ImGui::Checkbox(("##ws-in-" + p.machine_id).c_str(),
                                &in_on)
                && is_member) {
                if (in_on) v.form_input_members.insert(p.machine_id);
                else       v.form_input_members.erase(p.machine_id);
            }
            ImGui::TableSetColumnIndex(2);
            bool cb_on = v.form_clipboard_members.count(p.machine_id) > 0;
            if (ImGui::Checkbox(("##ws-cb-" + p.machine_id).c_str(),
                                &cb_on)
                && is_member) {
                if (cb_on) v.form_clipboard_members.insert(p.machine_id);
                else       v.form_clipboard_members.erase(p.machine_id);
            }
            if (!is_member) ImGui::EndDisabled();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    // ── Clipboard ─────────────────────────────────────────────
    section_header("Clipboard");
    {
        static const char* kSizes[] = {
            "100 KB", "1 MB", "5 MB", "10 MB", "Unlimited"};
        labeled_combo("##ws-cb-max", "Max text size",
                      v.clipboard_max, kSizes, IM_ARRAYSIZE(kSizes));
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    wrapping_checkbox("##ws-cb-rich",
                      "Include rich text / images", v.clipboard_rich);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    wrapping_checkbox("##ws-cb-files",
                      "Include files", v.clipboard_files);

    // ── Cursor ────────────────────────────────────────────────
    section_header("Cursor");
    labeled_int_input("##ws-cursor-edge", "Edge margin (px)",
                      v.cursor_edge_margin, 4, 50);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    wrapping_checkbox(
        "##ws-cursor-mod",
        "Hold Ctrl+Shift to move the cursor to another computer",
        v.cursor_require_modifier);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    wrapping_checkbox(
        "##ws-cursor-block",
        "Block OS hotkeys from forwarding (Win+L, Ctrl+Alt+Del, …)",
        v.cursor_block_hotkeys);

    // ── Auto-unlock ───────────────────────────────────────────
    section_header("Auto-unlock");
    {
        static const char* kIdle[] = {"Off", "5 min", "15 min", "1 hour"};
        labeled_combo("##ws-auto-unlock", "After idle",
                      v.auto_unlock, kIdle, IM_ARRAYSIZE(kIdle));
    }

    // Save requires:
    //   * non-blank name,
    //   * at least 2 members,
    //   * at least one member with Input enabled (covers both
    //     mouse + keyboard sharing — one checkbox now controls
    //     both since separate Cursor / Keyboard sets always
    //     ended up identical in practice).
    const std::string trimmed_name(v.name_buffer.data());
    const bool name_ok =
        !trimmed_name.empty()
        && trimmed_name.find_first_not_of(' ') != std::string::npos;
    const bool members_ok = v.form_members.size() >= 2;
    const bool input_ok   = !v.form_input_members.empty();
    const bool dirty      = form_dirty(v);
    // Save is active only when something has actually changed
    // *and* the workspace is valid — otherwise an unedited form
    // could spam redundant LWW bumps across the mesh, and an
    // invalid form would silently land a broken workspace.
    const bool can_save = name_ok && members_ok && input_ok && dirty;

    const char* gate_msg = nullptr;
    if (!members_ok) {
        gate_msg = "Pick at least 2 computers to save.";
    } else if (!input_ok) {
        gate_msg = "At least one computer must have Input enabled.";
    }
    if (gate_msg) {
        ImGui::Dummy(ImVec2(0.0f, theme::space::xs));
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_faint, "%s", gate_msg);
        ImGui::PopFont();
    }

    ImGui::Dummy(ImVec2(0.0f, theme::space::sm));

    // Action row: Save · Reset · (Delete if editing). Save persists
    // and *stays* on the form — the user leaves via the back arrow
    // at the top of the manager. After a Create-Save, the form
    // transitions to Edit mode of the new workspace so a second
    // Save updates rather than creating a duplicate.
    if (!can_save) ImGui::BeginDisabled();
    if (pill_button("Save##ws-form", PillVariant::Primary)) {
        const auto settings = settings_from_form(v);
        if (editing) {
            orch.rename_workspace(v.editing_id, trimmed_name);
            orch.set_workspace_members(v.editing_id,
                                        v.form_members,
                                        v.form_input_members,
                                        v.form_clipboard_members);
            orch.set_workspace_settings(v.editing_id, settings);
        } else {
            std::string new_id =
                orch.create_workspace(trimmed_name,
                                       v.form_members,
                                       v.form_input_members,
                                       v.form_clipboard_members);
            if (!new_id.empty()) {
                orch.set_workspace_settings(new_id, settings);
                v.mode       = Mode::Edit;
                v.editing_id = std::move(new_id);
            }
        }
        // Re-baseline the form so the dirty gate flips back to
        // false — Save is greyed out again until the user makes
        // another change.
        if (auto saved = orch.workspace(v.editing_id); saved) {
            seed_form_from(*saved, v);
        }
    }
    if (!can_save) ImGui::EndDisabled();

    ImGui::SameLine();
    if (pill_button("Reset##ws-form", PillVariant::Secondary)) {
        // Reset = discard pending edits, restore the form to
        // the last known state — either the orchestrator's
        // stored workspace (Edit) or the Create-mode pristine
        // defaults. Re-seeding via seed_form_from / start_create
        // also resets the dirty baseline, so Save goes back to
        // disabled until the user changes something again.
        if (editing) {
            if (auto ws = orch.workspace(v.editing_id); ws) {
                seed_form_from(*ws, v);
            }
        } else {
            start_create(v);
        }
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
