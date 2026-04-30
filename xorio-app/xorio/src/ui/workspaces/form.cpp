/// @file form.cpp
/// @brief Inline Create / Edit form + Add / remove computers
/// picker for the Workspaces-tab manager.

#include "ui/workspaces/form.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xorio::ui::workspaces {

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
    v.locked                   = ws.locked;
    v.lock_unlock_after_h      = static_cast<int>(ws.lock_unlock_after_h);
    v.master_locked            = ws.master_locked;
    v.master_lock_unlock_after_h =
        static_cast<int>(ws.master_lock_unlock_after_h);

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
    v.baseline_locked                  = v.locked;
    v.baseline_lock_unlock_after_h     = v.lock_unlock_after_h;
    v.baseline_master_locked           = v.master_locked;
    v.baseline_master_lock_unlock_after_h = v.master_lock_unlock_after_h;
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
    if (v.baseline_locked               != v.locked) return true;
    if (v.baseline_lock_unlock_after_h  != v.lock_unlock_after_h) return true;
    if (v.baseline_master_locked        != v.master_locked) return true;
    if (v.baseline_master_lock_unlock_after_h
        != v.master_lock_unlock_after_h) return true;
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
    s.locked                 = v.locked;
    s.lock_unlock_after_h    = static_cast<std::uint32_t>(
        std::max(0, v.lock_unlock_after_h));
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
    v.baseline_locked                  = v.locked;
    v.baseline_lock_unlock_after_h     = v.lock_unlock_after_h;
    v.baseline_master_locked           = v.master_locked;
    v.baseline_master_lock_unlock_after_h = v.master_lock_unlock_after_h;
}

void start_edit(const orchestrator::Workspace& ws, ViewState& v) {
    v.reset();
    v.mode       = Mode::Edit;
    v.editing_id = ws.id;
    seed_form_from(ws, v);
}

bool render_form(orchestrator::IOrchestrator& orch, ViewState& v) {
    const bool editing = (v.mode == Mode::Edit);
    // Race-bailout: if a remote peer locked this workspace while
    // the form was open, the local PC may no longer have edit
    // permission. Saving from a stale form would clobber the
    // incoming lock with the form's old "locked = false" snapshot,
    // so close the form and drop the unsaved buffer the moment
    // the workspace becomes uneditable. The user lands back on
    // the workspaces list and sees the lock state as it stands.
    if (editing && !v.editing_id.empty()) {
        if (auto ws = orch.workspace(v.editing_id); ws) {
            if (!orchestrator::can_edit_workspace(
                    *ws, orch.local_machine_id())) {
                v.reset();
                return false;
            }
        } else {
            // Workspace tombstoned remotely while we were editing
            // — same exit path, same buffer-dropped semantics.
            v.reset();
            return false;
        }
    }

    // Steal-detection: any PC currently in our form_members buffer
    // that just landed in another workspace was claimed by a
    // remote Save while we were still composing this one. Drop
    // them from the buffer + caps, surface their names in the
    // notification banner. Looks at every workspace in the
    // catalogue (including ones we don't edit) so we don't miss
    // the case where a third peer's Save is the one stealing.
    {
        const auto all_ws = orch.workspaces();
        std::unordered_map<std::string, std::string> claimed_by;
        for (const auto& ws : all_ws) {
            if (editing && ws.id == v.editing_id) continue;
            for (const auto& mid : ws.members) {
                claimed_by[mid] = ws.name.empty() ? ws.id : ws.name;
            }
        }
        const auto peers = orch.peers();
        std::unordered_map<std::string, std::string> peer_name;
        for (const auto& p : peers) {
            peer_name[p.machine_id] = p.display_name.empty()
                ? p.machine_id : p.display_name;
        }
        std::vector<std::string> stolen_now;
        for (auto it = v.form_members.begin();
             it != v.form_members.end(); ) {
            const auto cb = claimed_by.find(*it);
            if (cb != claimed_by.end()) {
                const auto pn = peer_name.find(*it);
                stolen_now.push_back(pn != peer_name.end()
                                      ? pn->second : *it);
                v.form_input_members.erase(*it);
                v.form_clipboard_members.erase(*it);
                it = v.form_members.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& n : stolen_now) {
            v.stolen_member_names.push_back(std::move(n));
        }
    }

    // Notification banner — orange strip at the top of the form
    // listing every PC the steal-detection just yanked from the
    // member buffer. Persists for the lifetime of the form so
    // the user can scroll back up and read it; clears on Save /
    // Cancel via v.reset().
    if (!v.stolen_member_names.empty()) {
        ImGui::PushFont(theme::font::body_sm);
        std::string msg;
        for (std::size_t i = 0; i < v.stolen_member_names.size(); ++i) {
            if (i != 0) msg += ", ";
            msg += v.stolen_member_names[i];
        }
        ImGui::TextColored(theme::palette::amber,
                           "Removed: %s — added to another workspace.",
                           msg.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, theme::space::sm));
    }
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

    // ── Computers ─────────────────────────────────────────────
    //
    // Members table shows only the PCs that are currently in the
    // workspace; non-members live in the "Add computer" picker
    // below the list. Each member row carries Input + Clipboard
    // capability checkboxes (Input is the unified mouse +
    // keyboard toggle — separate Cursor / Keyboard checkboxes
    // proved redundant since the user always wanted both or
    // neither) and a Remove button that yanks the PC out of all
    // three sets at once.
    auto peers = orch.peers();
    std::sort(peers.begin(), peers.end(),
              [](const orchestrator::Peer& a, const orchestrator::Peer& b) {
                  const std::string& an = a.display_name.empty()
                      ? a.machine_id : a.display_name;
                  const std::string& bn = b.display_name.empty()
                      ? b.machine_id : b.display_name;
                  return an < bn;
              });

    // Index for fast machine_id → Peer lookup. Members in the
    // form might not all be in `peers()` (offline / disconnected
    // peers still belong to the workspace), so the row renderer
    // falls back to the machine_id as the label.
    std::unordered_map<std::string, const orchestrator::Peer*> peer_by_id;
    peer_by_id.reserve(peers.size());
    for (const auto& p : peers) peer_by_id[p.machine_id] = &p;

    // Sorted member machine_ids by display name — keeps the
    // member list stable instead of jumping around each frame.
    std::vector<std::string> member_ids(v.form_members.begin(),
                                        v.form_members.end());
    std::sort(member_ids.begin(), member_ids.end(),
              [&](const std::string& a, const std::string& b) {
                  auto label_for = [&](const std::string& mid) -> std::string {
                      auto it = peer_by_id.find(mid);
                      if (it == peer_by_id.end()) return mid;
                      return it->second->display_name.empty()
                                 ? it->second->machine_id
                                 : it->second->display_name;
                  };
                  return label_for(a) < label_for(b);
              });

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_SizingFixedFit
        | ImGuiTableFlags_NoBordersInBody
        | ImGuiTableFlags_PadOuterX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 4.0f));
    if (member_ids.empty()) {
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_faint,
                           "No computers in this workspace yet — "
                           "use Add / remove computers below.");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, theme::space::xs));
    } else if (ImGui::BeginTable("##ws-members", 3, kTableFlags)) {
        ImGui::TableSetupColumn("Computer",
                                 ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Input",
                                 ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Clipboard",
                                 ImGuiTableColumnFlags_WidthFixed, 78.0f);

        const char* headers[] = {"Computer", "Input", "Clipboard"};
        ImGui::TableNextRow();
        for (int c = 0; c < 3; ++c) {
            ImGui::TableSetColumnIndex(c);
            ImGui::PushFont(theme::font::body_sm);
            ImGui::TextColored(theme::palette::paper_muted, "%s", headers[c]);
            ImGui::PopFont();
        }

        for (const auto& mid : member_ids) {
            auto it = peer_by_id.find(mid);
            const std::string label = (it != peer_by_id.end()
                && !it->second->display_name.empty())
                ? it->second->display_name : mid;
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(theme::palette::paper_text,
                               "%s", label.c_str());

            ImGui::TableSetColumnIndex(1);
            bool in_on = v.form_input_members.count(mid) > 0;
            if (ImGui::Checkbox(("##ws-in-" + mid).c_str(), &in_on)) {
                if (in_on) v.form_input_members.insert(mid);
                else       v.form_input_members.erase(mid);
            }

            ImGui::TableSetColumnIndex(2);
            bool cb_on = v.form_clipboard_members.count(mid) > 0;
            if (ImGui::Checkbox(("##ws-cb-" + mid).c_str(), &cb_on)) {
                if (cb_on) v.form_clipboard_members.insert(mid);
                else       v.form_clipboard_members.erase(mid);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0.0f, theme::space::sm));

    // "Add / remove computers" pill flips the form into a
    // sibling view — the host (activity.cpp) sees show_member_
    // picker, swaps the manager-card title to "Add / remove
    // computers", and routes the back arrow to close the picker
    // only (not the whole form). Membership state lives on
    // ViewState, so toggles in the picker show up here on the
    // next form render.
    if (pill_button("Add / remove computers##ws-members-toggle",
                     PillVariant::Secondary)) {
        v.show_member_picker = true;
    }

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
                      "Include files (Ctrl+C / Ctrl+V)",
                      v.clipboard_files);

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

    // ── Locks ─────────────────────────────────────────────────
    // Lock = members-only edit (non-members see the workspace +
    // layout but can't change it). Master-Lock = only the peer
    // who flipped it on can edit; everyone else view-only. Each
    // has a per-row "Unlock After idle (h)" — 0 means the lock
    // sticks until manually toggled off.
    section_header("Locks");
    auto lock_row = [](const char* cb_id, const char* label, bool& on,
                       const char* h_id, int& hours) {
        ImGui::Checkbox(cb_id, &on);
        ImGui::SameLine();
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_text, "%s", label);
        ImGui::PopFont();
        ImGui::SameLine(220.0f);
        if (!on) ImGui::BeginDisabled();
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_muted, "Unlock after idle (h):");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60.0f);
        int v = hours;
        if (ImGui::InputInt(h_id, &v, 0, 0)) {
            hours = std::max(0, v);
        }
        if (!on) ImGui::EndDisabled();
    };
    lock_row("##ws-lock", "Lock",
             v.locked, "##ws-lock-h", v.lock_unlock_after_h);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    lock_row("##ws-master-lock", "Master-Lock",
             v.master_locked, "##ws-master-lock-h",
             v.master_lock_unlock_after_h);

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
        const std::uint32_t master_h = static_cast<std::uint32_t>(
            std::max(0, v.master_lock_unlock_after_h));
        if (editing) {
            orch.rename_workspace(v.editing_id, trimmed_name);
            orch.set_workspace_members(v.editing_id,
                                        v.form_members,
                                        v.form_input_members,
                                        v.form_clipboard_members);
            orch.set_workspace_settings(v.editing_id, settings);
            // Master-Lock has owner-attribution; only emit the
            // toggle on actual change so the form's idle frames
            // don't keep rewriting `master_locked_by`.
            if (v.master_locked != v.baseline_master_locked
                || v.master_lock_unlock_after_h
                   != v.baseline_master_lock_unlock_after_h) {
                orch.set_workspace_master_lock(v.editing_id,
                                                v.master_locked, master_h);
            }
        } else {
            std::string new_id =
                orch.create_workspace(trimmed_name,
                                       v.form_members,
                                       v.form_input_members,
                                       v.form_clipboard_members);
            if (!new_id.empty()) {
                orch.set_workspace_settings(new_id, settings);
                if (v.master_locked || master_h != 0) {
                    orch.set_workspace_master_lock(new_id,
                                                    v.master_locked,
                                                    master_h);
                }
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

void render_member_picker(orchestrator::IOrchestrator& orch,
                           ViewState& v) {
    // Sort peers by display name so the selectable list reads
    // alphabetically. Same lookup the form uses; we deliberately
    // duplicate the sort instead of sharing a helper so the two
    // views stay independently ownable.
    auto peers = orch.peers();
    std::sort(peers.begin(), peers.end(),
              [](const orchestrator::Peer& a,
                  const orchestrator::Peer& b) {
                  const std::string& an = a.display_name.empty()
                      ? a.machine_id : a.display_name;
                  const std::string& bn = b.display_name.empty()
                      ? b.machine_id : b.display_name;
                  return an < bn;
              });

    // PCs already in another workspace can't be ticked here —
    // the rule is one workspace per PC, and the actual move only
    // happens on Save by the picker's owner. We render those
    // peers as disabled with a "(in <workspace>)" suffix so the
    // user sees why they're unavailable.
    const bool editing = (v.mode == Mode::Edit);
    std::unordered_map<std::string, std::string> claimed_by;
    for (const auto& ws : orch.workspaces()) {
        if (editing && ws.id == v.editing_id) continue;
        for (const auto& mid : ws.members) {
            claimed_by[mid] = ws.name.empty() ? ws.id : ws.name;
        }
    }

    if (peers.empty()) {
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_faint,
                           "No computers visible on the LAN yet.");
        ImGui::PopFont();
        return;
    }

    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_muted,
                       "Tick a computer to add it to the workspace, "
                       "untick to remove. Changes apply immediately; "
                       "use the back arrow to return.");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, theme::space::sm));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(0.0f, 6.0f));
    for (const auto& p : peers) {
        const std::string label = p.display_name.empty()
                                  ? p.machine_id : p.display_name;
        const auto cb_it = claimed_by.find(p.machine_id);
        const bool taken = cb_it != claimed_by.end();
        bool is_member = v.form_members.count(p.machine_id) > 0;
        if (taken) ImGui::BeginDisabled();
        if (ImGui::Checkbox(
                ("##ws-pick-" + p.machine_id).c_str(),
                &is_member)) {
            // Newly-added members default to Input + Clipboard
            // caps on (most common intent); removing drops all
            // three sets atomically so a re-add starts clean.
            if (is_member) {
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
        ImGui::TextColored(taken ? theme::palette::paper_faint
                                  : theme::palette::paper_text,
                           "%s", label.c_str());
        if (taken) {
            ImGui::SameLine(0.0f, theme::space::sm);
            ImGui::TextColored(theme::palette::paper_faint,
                               "(in %s)", cb_it->second.c_str());
            ImGui::EndDisabled();
        }
    }
    ImGui::PopStyleVar();
}

}  // namespace xorio::ui::workspaces
