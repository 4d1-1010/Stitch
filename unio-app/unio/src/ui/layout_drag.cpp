/// @file layout_drag.cpp
/// @brief Drag-to-rearrange interaction for the Layout tab.
///
/// Scope: drag lifecycle helpers + AABB collision avoidance +
/// Apply / Revert footer. The canvas drawing lives in
/// `layout.cpp` and supplies the screen-space pan + scale + the
/// per-peer X offset map so this TU's collision math sees the
/// exact rectangles the user sees.

#include "ui/layout_drag.hpp"

#include "imgui.h"

#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstddef>

namespace unio_ui::ui::layout {

ImVec2 saved_offset(const DragState& drag, const DisplayKey& key) {
    if (auto it = drag.overrides.find(key); it != drag.overrides.end()) {
        return it->second;
    }
    return ImVec2(0.0f, 0.0f);
}

void try_start_drag(const DisplayKey& key,
                    float sx, float sy, float sw, float sh,
                    ImVec2 mouse, DragState& drag) {
    if (drag.active) return;
    const bool over = mouse.x >= sx && mouse.x < sx + sw
                   && mouse.y >= sy && mouse.y < sy + sh;
    if (!over) return;
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;

    drag.active      = true;
    drag.target      = key;
    drag.grab_offset = ImVec2(mouse.x - sx, mouse.y - sy);
    drag.live_delta  = ImVec2(0.0f, 0.0f);
}

void update_drag_with_collision(
    const std::vector<orchestrator::Display>& displays,
    float scale, float pan_x, float pan_y,
    ImVec2 mouse,
    const std::map<std::string, std::int32_t>& peer_render_offset,
    DragState& drag) {

    // Locate the dragged display so we know its size + natural
    // screen anchor (origin + saved override).
    const orchestrator::Display* target_d = nullptr;
    for (const auto& d : displays) {
        if (DisplayKey{d.machine_id, d.monitor_id} == drag.target) {
            target_d = &d;
            break;
        }
    }
    if (target_d == nullptr) return;

    auto target_offset_it = peer_render_offset.find(target_d->machine_id);
    if (target_offset_it == peer_render_offset.end()) return;

    const float sw      = target_d->width  * scale;
    const float sh      = target_d->height * scale;
    const float sx_orig = pan_x +
        (target_d->global_x + target_offset_it->second) * scale;
    const float sy_orig = pan_y + target_d->global_y * scale;
    const ImVec2 committed = saved_offset(drag, drag.target);

    // Candidate position from the mouse — the rect should track
    // the click point inside it.
    float sx_cand = mouse.x - drag.grab_offset.x;
    float sy_cand = mouse.y - drag.grab_offset.y;

    // AABB collision pass against every other rectangle. Iterate
    // up to four times so a resolution that creates a new overlap
    // gets re-resolved within the same frame.
    for (int pass = 0; pass < 4; ++pass) {
        bool moved = false;
        for (const auto& o : displays) {
            const DisplayKey ok{o.machine_id, o.monitor_id};
            if (ok == drag.target) continue;

            auto o_off_it = peer_render_offset.find(o.machine_id);
            if (o_off_it == peer_render_offset.end()) continue;

            const float ox_orig = pan_x + (o.global_x + o_off_it->second) * scale;
            const float oy_orig = pan_y + o.global_y * scale;
            const ImVec2 o_off  = saved_offset(drag, ok);
            const float ox = ox_orig + o_off.x;
            const float oy = oy_orig + o_off.y;
            const float ow = o.width  * scale;
            const float oh = o.height * scale;

            const bool overlap =
                sx_cand < ox + ow && sx_cand + sw > ox &&
                sy_cand < oy + oh && sy_cand + sh > oy;
            if (!overlap) continue;

            // Pick the resolution axis with the smaller push so
            // the rect "slides along" the obstacle the user is
            // hugging instead of teleporting around it.
            const float push_l = sx_cand + sw - ox;
            const float push_r = (ox + ow) - sx_cand;
            const float push_u = sy_cand + sh - oy;
            const float push_d = (oy + oh) - sy_cand;
            const float resolve_x = std::min(push_l, push_r);
            const float resolve_y = std::min(push_u, push_d);
            if (resolve_x < resolve_y) {
                sx_cand += (push_r < push_l) ? push_r : -push_l;
            } else {
                sy_cand += (push_d < push_u) ? push_d : -push_u;
            }
            moved = true;
        }
        if (!moved) break;
    }

    drag.live_delta.x = sx_cand - sx_orig - committed.x;
    drag.live_delta.y = sy_cand - sy_orig - committed.y;
}

void commit_drag_release(DragState& drag) {
    if (!drag.active) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

    auto& slot = drag.overrides[drag.target];
    slot.x += drag.live_delta.x;
    slot.y += drag.live_delta.y;

    drag.active     = false;
    drag.live_delta = ImVec2(0.0f, 0.0f);
}

void render_drag_footer(DragState& drag) {
    const bool dirty = !drag.overrides.empty();

    ImGui::PushFont(theme::font::body_sm);
    if (dirty) {
        const std::size_t n = drag.overrides.size();
        ImGui::TextColored(theme::palette::amber,
                           "%zu unsaved layout change%s",
                           n, n == 1 ? "" : "s");
    } else {
        ImGui::TextColored(theme::palette::paper_muted,
                           "Drag a display to rearrange. Adjacency "
                           "only — display routing isn't editable.");
    }
    ImGui::PopFont();
    ImGui::SameLine();

    const float btn_w =
        ImGui::CalcTextSize("Apply").x  + 32.0f
      + ImGui::CalcTextSize("Revert").x + 32.0f
      + theme::space::sm;
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX()
        + ImGui::GetContentRegionAvail().x - btn_w);

    if (!dirty) ImGui::BeginDisabled();
    if (pill_button("Apply##layout-apply", PillVariant::Primary)) {
        // Persistence rides a future LayoutRecord through the
        // mesh CRDT. For now Apply just clears the pending map;
        // the visual state survives until next reload.
        drag.overrides.clear();
    }
    ImGui::SameLine();
    if (pill_button("Revert##layout-revert", PillVariant::Secondary)) {
        drag.overrides.clear();
    }
    if (!dirty) ImGui::EndDisabled();
}

}  // namespace unio_ui::ui::layout
