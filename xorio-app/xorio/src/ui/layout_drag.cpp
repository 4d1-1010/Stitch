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

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstddef>
#include <unordered_set>

namespace xorio::ui::layout {

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
    drag.scale_start = drag.last_scale > 0.0f ? drag.last_scale : 1.0f;
}

void update_drag_with_collision(
    const std::vector<orchestrator::Display>& displays,
    const std::vector<orchestrator::Display>& obstacles,
    float scale, float pan_x, float pan_y,
    ImVec2 canvas_min, ImVec2 canvas_max,
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
    // Saved offsets are in display units; multiply by current
    // scale to compare against screen-space candidate positions.
    const ImVec2 committed_disp   = saved_offset(drag, drag.target);
    const float committed_screen_x = committed_disp.x * scale;
    const float committed_screen_y = committed_disp.y * scale;

    // A rect wider/taller than the canvas would invert min/max
    // — pick min in that case so the rect anchors at the
    // top/left edge instead of producing a NaN.
    const float min_x = canvas_min.x;
    const float max_x = std::max(canvas_min.x, canvas_max.x - sw);
    const float min_y = canvas_min.y;
    const float max_y = std::max(canvas_min.y, canvas_max.y - sh);

    // Where the cursor wants the rect. When the cursor is inside
    // the canvas the rect tracks 1:1; when it crosses outside,
    // we still let the rect's display position creep further (so
    // the layout can be extended past what fits in the canvas)
    // but at a fixed rate per frame so the auto-fit doesn't zoom
    // out at "really fast" cursor velocity. The rect itself
    // stays at the canvas edge in screen — clamping happens in
    // draw_displays so the visual stays put while live_delta
    // (used by fit_displays) gradually grows.
    const float sx_desired = mouse.x - drag.grab_offset.x;
    const float sy_desired = mouse.y - drag.grab_offset.y;
    const bool cursor_inside =
        sx_desired >= min_x && sx_desired <= max_x &&
        sy_desired >= min_y && sy_desired <= max_y;

    float sx_cand, sy_cand;
    if (cursor_inside) {
        sx_cand = sx_desired;
        sy_cand = sy_desired;
    } else {
        constexpr float kEdgeRate = 4.0f;  // px per frame
        const float prev_sx = sx_orig + committed_screen_x + drag.live_delta.x;
        const float prev_sy = sy_orig + committed_screen_y + drag.live_delta.y;
        const float dx = sx_desired - prev_sx;
        const float dy = sy_desired - prev_sy;
        sx_cand = prev_sx + std::clamp(dx, -kEdgeRate, kEdgeRate);
        sy_cand = prev_sy + std::clamp(dy, -kEdgeRate, kEdgeRate);
    }

    auto overlaps_any = [&](float x, float y) -> bool {
        auto check_list = [&](const std::vector<orchestrator::Display>& list,
                              bool is_obstacle) -> bool {
            for (const auto& o : list) {
                const DisplayKey ok{o.machine_id, o.monitor_id};
                if (!is_obstacle && ok == drag.target) continue;
                auto o_off_it = peer_render_offset.find(o.machine_id);
                const std::int32_t poff = o_off_it != peer_render_offset.end()
                                            ? o_off_it->second
                                            : 0;
                const ImVec2 o_off = saved_offset(drag, ok);
                const float ox = pan_x + (o.global_x + poff) * scale
                               + o_off.x * scale;
                const float oy = pan_y + o.global_y * scale
                               + o_off.y * scale;
                const float ow = o.width  * scale;
                const float oh = o.height * scale;
                if (x < ox + ow && x + sw > ox && y < oy + oh && y + sh > oy) {
                    return true;
                }
            }
            return false;
        };
        return check_list(displays, false) || check_list(obstacles, true);
    };

    // AABB resolution pass: push out of overlap. No canvas clamp
    // — the screen-side clamp lives in draw_displays so we don't
    // lose live_delta growth here. If a position can't be made
    // overlap-free, freeze the rect at its previous valid spot
    // rather than letting it land on top of another rect.
    auto resolve_against = [&](const std::vector<orchestrator::Display>& list,
                                bool is_obstacle, bool& any_overlap) {
        for (const auto& o : list) {
            const DisplayKey ok{o.machine_id, o.monitor_id};
            if (!is_obstacle && ok == drag.target) continue;
            auto o_off_it = peer_render_offset.find(o.machine_id);
            const std::int32_t poff = o_off_it != peer_render_offset.end()
                                        ? o_off_it->second
                                        : 0;
            const float ox_orig = pan_x + (o.global_x + poff) * scale;
            const float oy_orig = pan_y + o.global_y * scale;
            const ImVec2 o_off  = saved_offset(drag, ok);
            const float ox = ox_orig + o_off.x * scale;
            const float oy = oy_orig + o_off.y * scale;
            const float ow = o.width  * scale;
            const float oh = o.height * scale;
            const bool overlap =
                sx_cand < ox + ow && sx_cand + sw > ox &&
                sy_cand < oy + oh && sy_cand + sh > oy;
            if (!overlap) continue;
            any_overlap = true;
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
        }
    };

    // AABB resolution pass: push out of overlap. No canvas clamp
    // — the screen-side clamp lives in draw_displays so we don't
    // lose live_delta growth here. If a position can't be made
    // overlap-free, freeze the rect at its previous valid spot
    // rather than letting it land on top of another rect.
    constexpr int kMaxPasses = 8;
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        bool any_overlap = false;
        resolve_against(displays,  /*is_obstacle=*/false, any_overlap);
        resolve_against(obstacles, /*is_obstacle=*/true,  any_overlap);
        if (!any_overlap) break;
    }

    if (overlaps_any(sx_cand, sy_cand)) return;

    drag.live_delta.x = sx_cand - sx_orig - committed_screen_x;
    drag.live_delta.y = sy_cand - sy_orig - committed_screen_y;
}

void commit_drag_release(DragState& drag) {
    if (!drag.active) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

    // Convert live_delta from screen pixels to display units
    // before folding into the saved override — overrides live in
    // display space so the rect's committed position is invariant
    // under canvas pan / scale changes during the smoothing tail.
    const float scale = drag.last_scale > 0.0f ? drag.last_scale : 1.0f;
    auto& slot = drag.overrides[drag.target];
    slot.x += drag.live_delta.x / scale;
    slot.y += drag.live_delta.y / scale;

    drag.active     = false;
    drag.live_delta = ImVec2(0.0f, 0.0f);
}

void render_layout_footer(orchestrator::IOrchestrator& orch,
                          DragState& drag,
                          bool disabled,
                          const ApplyContext& ctx) {
    const bool dirty = !drag.overrides.empty();

    if (disabled) ImGui::BeginDisabled();

    // Identify sits on the left — the action is read-only and
    // doesn't depend on dirty state. The orchestrator fans the
    // request across the mesh: every peer's announce carries the
    // bumped counter, listeners fire their own local overlay
    // when they observe the bump, and the click also fires the
    // overlay on the local PC through the on_identify_request
    // callback. End result: every monitor in the mesh flashes
    // its number simultaneously.
    if (pill_button("Identify displays##layout-identify",
                     PillVariant::Secondary)) {
        orch.request_identify();
    }
    ImGui::SameLine();

    ImGui::PushFont(theme::font::body_sm);
    if (dirty) {
        const std::size_t n = drag.overrides.size();
        ImGui::TextColored(theme::palette::amber,
                           "%zu unsaved layout change%s",
                           n, n == 1 ? "" : "s");
    } else {
        ImGui::TextColored(theme::palette::paper_muted,
                           "Drag a display to rearrange.");
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

    const bool apply_revert_disabled = disabled || !dirty;
    if (apply_revert_disabled) ImGui::BeginDisabled();
    if (pill_button("Apply##layout-apply", PillVariant::Primary)) {
        // Convert each display's render-time base position +
        // drag delta back into a mesh-global position and write
        // them into the active workspace's layout. Sync rides
        // the existing LWW path, so every peer sees the same
        // arrangement after the next announce tick.
        //
        // The canvas only shows displays for online members (an
        // offline peer's monitors are hidden until they reconnect)
        // — but their saved layout entries must still survive an
        // Apply from someone else. We start the new layout from
        // the existing workspace's entries belonging to OFFLINE
        // members, then add the UI's edited online entries on top.
        // When the offline member reconnects, the LWW merge picks
        // our newer version_ns so the online edits propagate, and
        // their own (untouched) entries come along for the ride.
        if (!ctx.workspace_id.empty()) {
            std::unordered_set<std::string> online_members;
            for (const auto& p : orch.peers()) {
                if (p.online) online_members.insert(p.machine_id);
            }
            online_members.insert(orch.local_machine_id());

            std::vector<orchestrator::DisplayLayoutEntry> entries;
            if (auto ws = orch.workspace(ctx.workspace_id); ws) {
                for (const auto& e : ws->layout) {
                    if (online_members.count(e.machine_id) == 0) {
                        entries.push_back(e);
                    }
                }
            }
            entries.reserve(entries.size() + ctx.displays.size());
            for (const auto& d : ctx.displays) {
                orchestrator::DisplayLayoutEntry e;
                e.machine_id = d.machine_id;
                e.monitor_id = d.monitor_id;
                e.global_x   = d.global_x;
                e.global_y   = d.global_y;
                const auto k = DisplayKey{d.machine_id, d.monitor_id};
                if (auto it = drag.overrides.find(k);
                    it != drag.overrides.end()) {
                    // Overrides are already in display units —
                    // no scale conversion needed.
                    e.global_x += static_cast<std::int32_t>(it->second.x);
                    e.global_y += static_cast<std::int32_t>(it->second.y);
                }
                entries.push_back(std::move(e));
            }
            orch.set_workspace_layout(ctx.workspace_id, entries);
        }
        drag.overrides.clear();
    }
    ImGui::SameLine();
    if (pill_button("Revert##layout-revert", PillVariant::Secondary)) {
        drag.overrides.clear();
    }
    if (apply_revert_disabled) ImGui::EndDisabled();

    if (disabled) ImGui::EndDisabled();
}

}  // namespace xorio::ui::layout
