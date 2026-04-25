/// @file layout.cpp
/// @brief Two-row layout canvas: PC strip + display strip + identity routes.

#include "ui/layout.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/machine_color.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace unio_ui::ui::layout {

namespace {

constexpr float kTopBandY        = 16.0f;
constexpr float kTopBandH        = 84.0f;
constexpr float kBottomBandY     = kTopBandY + kTopBandH + 32.0f;
constexpr float kPcNodeW         = 200.0f;
constexpr float kPcNodeH         = 56.0f;
constexpr float kNodeGap         = 28.0f;
constexpr float kBandPadX        = 40.0f;

/// @brief Maximum bottom-band scale (display-pixels → screen-pixels).
/// Used as the upper bound when the auto-fit calculation would
/// otherwise pick a larger zoom on tiny configurations.
constexpr float kBottomScaleMax  = 0.12f;
/// @brief Minimum scale before things become illegible. Below this
/// we accept clipping and stop shrinking.
constexpr float kBottomScaleMin  = 0.02f;

constexpr ImU32 kCanvasBg = IM_COL32(0xf8, 0xf9, 0xfc, 0xff);
constexpr ImU32 kGridLine = IM_COL32(0xed, 0xef, 0xf4, 0xff);

/// @brief Blend @p fg over @p bg at alpha @p a, producing an opaque colour.
ImVec4 blend(ImVec4 fg, float a, ImVec4 bg = {0.973f, 0.977f, 0.988f, 1.0f}) {
    return ImVec4(fg.x * a + bg.x * (1.0f - a),
                  fg.y * a + bg.y * (1.0f - a),
                  fg.z * a + bg.z * (1.0f - a),
                  1.0f);
}

/// @brief Hit-test record produced while drawing.
struct Node {
    enum class Kind { Pc, Display };
    Kind kind;
    std::string machine_id;
    std::string monitor_id;
    ImVec2 tl;
    ImVec2 br;
};

/// @brief Stable identity key for an override entry — `{machine_id,
/// monitor_id}`. Two displays on the same machine still need
/// distinct overrides.
using DisplayKey = std::pair<std::string, std::string>;

/// @brief Cross-frame state for the drag-to-rearrange interaction.
///
/// Overrides are stored in **screen-space** pixel deltas, applied
/// on top of the rectangle's natural global-coords-to-screen
/// position. Persisting them through the orchestrator (so adjacency
/// edits ride the mesh) is a follow-up — Apply / Revert today
/// only manipulate this in-memory map. The map is intentionally
/// `std::map` for stable iteration order.
struct DragState {
    bool                          active = false;  ///< Mouse is currently held.
    DisplayKey                    target;          ///< The rect being dragged.
    ImVec2                        grab_offset{0, 0};  ///< Mouse offset inside the rect at grab time.
    ImVec2                        live_delta{0, 0};   ///< Drag delta this frame.
    std::map<DisplayKey, ImVec2>  overrides;       ///< Committed deltas per display.
};

/// @brief Draw PC nodes as a horizontally-centred row.
void draw_top_band(ImDrawList* dl, const ImVec2& origin, float width,
                   const std::vector<std::string>& machines,
                   const std::string& active_machine,
                   std::vector<Node>& out_nodes) {
    if (machines.empty()) return;

    const float total_w = machines.size() * kPcNodeW
                        + (machines.size() - 1) * kNodeGap;
    float x = origin.x + std::max(kBandPadX + 70.0f, (width - total_w) * 0.5f);
    const float y = origin.y + kTopBandY;

    for (const std::string& mid : machines) {
        const ImVec4 color = machine_color(mid);
        const ImVec4 fill = blend(color, 0.22f);
        const ImVec4 border = (mid == active_machine)
                              ? theme::palette::lilac : color;
        const ImVec2 tl(x, y);
        const ImVec2 br(x + kPcNodeW, y + kPcNodeH);

        dl->AddRectFilled(tl, br,
                          ImGui::ColorConvertFloat4ToU32(fill),
                          theme::radius::sm);
        dl->AddRect(tl, br,
                    ImGui::ColorConvertFloat4ToU32(border),
                    theme::radius::sm, 0, 3.0f);
        dl->AddCircleFilled(
            ImVec2(tl.x + 22.0f, tl.y + kPcNodeH * 0.5f), 8.0f,
            ImGui::ColorConvertFloat4ToU32(color), 16);
        dl->AddText(ImVec2(tl.x + 40.0f, tl.y + kPcNodeH * 0.5f - 7.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::palette::paper_text),
                    mid.c_str());

        out_nodes.push_back({Node::Kind::Pc, mid, {}, tl, br});
        x += kPcNodeW + kNodeGap;
    }
}

/// @brief Draw display rectangles in global-desktop space with a
/// 120-pixel dot-grid behind them. Reads + mutates @p drag so
/// click / drag / release on a rect produces an override entry.
void draw_bottom_band(ImDrawList* dl, const ImVec2& origin,
                      float width, float height,
                      const std::vector<orchestrator::Display>& displays,
                      std::vector<Node>& out_nodes,
                      DragState& drag) {
    const float grid_y0 = origin.y + kBottomBandY + 20.0f;
    const float grid_y1 = origin.y + height - 10.0f;
    const float grid_x0 = origin.x + 20.0f;
    const float grid_x1 = origin.x + width - 20.0f;
    for (float gx = grid_x0; gx < grid_x1; gx += 120.0f) {
        dl->AddLine(ImVec2(gx, grid_y0), ImVec2(gx, grid_y1),
                    kGridLine, 1.0f);
    }
    for (float gy = grid_y0; gy < grid_y1; gy += 120.0f) {
        dl->AddLine(ImVec2(grid_x0, gy), ImVec2(grid_x1, gy),
                    kGridLine, 1.0f);
    }

    if (displays.empty()) return;

    // Compute the global-desktop bounding box across every peer's
    // displays so we can auto-fit the strip to the canvas width.
    // Without this, peers placed far apart in synthesized coords
    // overflow the visible region at the natural 0.12 scale.
    float min_x = static_cast<float>(displays[0].global_x);
    float max_x = static_cast<float>(displays[0].global_x + displays[0].width);
    float min_y = static_cast<float>(displays[0].global_y);
    float max_y = static_cast<float>(displays[0].global_y + displays[0].height);
    for (const auto& d : displays) {
        if (d.global_x < min_x) min_x = static_cast<float>(d.global_x);
        if (d.global_y < min_y) min_y = static_cast<float>(d.global_y);
        if (d.global_x + d.width > max_x) {
            max_x = static_cast<float>(d.global_x + d.width);
        }
        if (d.global_y + d.height > max_y) {
            max_y = static_cast<float>(d.global_y + d.height);
        }
    }
    const float data_w = std::max(1.0f, max_x - min_x);
    const float data_h = std::max(1.0f, max_y - min_y);

    // Available room inside the bottom band, after the side / top
    // padding the grid already reserves.
    const float band_w = std::max(1.0f, grid_x1 - grid_x0 - 40.0f);
    const float band_h = std::max(1.0f, grid_y1 - grid_y0 - 40.0f);

    // Isotropic scale — pick whichever axis is the binding
    // constraint, clamped to [kBottomScaleMin, kBottomScaleMax].
    float scale = std::min(kBottomScaleMax,
                           std::min(band_w / data_w, band_h / data_h));
    if (scale < kBottomScaleMin) scale = kBottomScaleMin;

    const float strip_w = data_w * scale;
    const float strip_h = data_h * scale;
    const float pan_x = origin.x + std::max(20.0f, (width - strip_w) * 0.5f)
                        - min_x * scale;
    const float pan_y = grid_y0 + std::max(0.0f, (band_h - strip_h) * 0.5f)
                        - min_y * scale;

    const ImVec2 mouse = ImGui::GetMousePos();
    const bool   mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    for (const auto& d : displays) {
        const DisplayKey key{d.machine_id, d.monitor_id};

        const float sx_orig = pan_x + d.global_x * scale;
        const float sy_orig = pan_y + d.global_y * scale;
        const float sw      = d.width  * scale;
        const float sh      = d.height * scale;

        // Final on-screen position = original + committed override
        // + live drag delta (only on the rect that's being dragged).
        ImVec2 offset(0.0f, 0.0f);
        if (auto it = drag.overrides.find(key); it != drag.overrides.end()) {
            offset = it->second;
        }
        if (drag.active && drag.target == key) {
            offset.x += drag.live_delta.x;
            offset.y += drag.live_delta.y;
        }
        const float sx = sx_orig + offset.x;
        const float sy = sy_orig + offset.y;

        const ImVec4 color = machine_color(d.machine_id);
        const ImVec4 fill = blend(color, 0.18f);

        dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh),
                          ImGui::ColorConvertFloat4ToU32(fill),
                          theme::radius::sm);
        dl->AddRect(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh),
                    ImGui::ColorConvertFloat4ToU32(color),
                    theme::radius::sm, 0, 3.0f);

        char num[8];
        std::snprintf(num, sizeof(num), "%d", d.number);
        const ImVec2 ns = ImGui::CalcTextSize(num);
        dl->AddText(ImVec2(sx + (sw - ns.x) * 0.5f,
                           sy + (sh - ns.y) * 0.5f - 6.0f),
                    ImGui::ColorConvertFloat4ToU32(theme::palette::paper_text),
                    num);

        if (sh > 34.0f && sw > 60.0f) {
            const std::string label = d.machine_id + ":" + d.monitor_id;
            const ImVec2 ls = ImGui::CalcTextSize(label.c_str());
            dl->AddText(ImVec2(sx + (sw - ls.x) * 0.5f,
                               sy + sh * 0.65f),
                        ImGui::ColorConvertFloat4ToU32(theme::palette::paper_muted),
                        label.c_str());
        }

        out_nodes.push_back({Node::Kind::Display, d.machine_id,
                             d.monitor_id,
                             ImVec2(sx, sy), ImVec2(sx + sw, sy + sh)});

        // Hit-test for drag start. Only when no other rect is
        // mid-drag, the click landed inside this rect, and the
        // mouse just transitioned to down.
        const bool over =
            mouse.x >= sx && mouse.x < sx + sw &&
            mouse.y >= sy && mouse.y < sy + sh;
        if (!drag.active && over
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            drag.active      = true;
            drag.target      = key;
            drag.grab_offset = ImVec2(mouse.x - sx, mouse.y - sy);
            drag.live_delta  = ImVec2(0.0f, 0.0f);
        }
    }

    // Update the live delta if a drag is in progress.
    if (drag.active && mouse_down) {
        // The grab offset captured the mouse position relative to
        // the rect at click time; translate the rect so the cursor
        // tracks that same point. Subtracting the existing
        // committed override keeps stacked drags additive.
        for (const auto& d : displays) {
            const DisplayKey key{d.machine_id, d.monitor_id};
            if (key != drag.target) continue;
            const float sx_orig = pan_x + d.global_x * scale;
            const float sy_orig = pan_y + d.global_y * scale;
            ImVec2 committed(0.0f, 0.0f);
            if (auto it = drag.overrides.find(key); it != drag.overrides.end()) {
                committed = it->second;
            }

            const float sw = d.width  * scale;
            const float sh = d.height * scale;
            float sx_cand = mouse.x - drag.grab_offset.x;
            float sy_cand = mouse.y - drag.grab_offset.y;

            // Resolve overlaps with every other rectangle by
            // pushing the candidate along the AABB axis with the
            // smaller correction. Iterating a few times catches
            // the case where one resolution introduces a new
            // overlap with a third rect; a small iteration cap
            // keeps the loop bounded if the layout is genuinely
            // packed.
            for (int pass = 0; pass < 4; ++pass) {
                bool moved = false;
                for (const auto& o : displays) {
                    const DisplayKey ok{o.machine_id, o.monitor_id};
                    if (ok == key) continue;

                    const float ox_orig = pan_x + o.global_x * scale;
                    const float oy_orig = pan_y + o.global_y * scale;
                    ImVec2 o_off(0.0f, 0.0f);
                    if (auto it = drag.overrides.find(ok);
                        it != drag.overrides.end()) {
                        o_off = it->second;
                    }
                    const float ox = ox_orig + o_off.x;
                    const float oy = oy_orig + o_off.y;
                    const float ow = o.width  * scale;
                    const float oh = o.height * scale;

                    const bool overlap =
                        sx_cand < ox + ow && sx_cand + sw > ox &&
                        sy_cand < oy + oh && sy_cand + sh > oy;
                    if (!overlap) continue;

                    // AABB resolution: pick the axis whose push
                    // distance is smaller so the rect "slides
                    // along" the obstacle the user is hugging.
                    const float push_l = sx_cand + sw - ox;       // → push left
                    const float push_r = (ox + ow) - sx_cand;     // → push right
                    const float push_u = sy_cand + sh - oy;       // → push up
                    const float push_d = (oy + oh) - sy_cand;     // → push down
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
            break;
        }
    }
}

/// @brief If a drag was in progress and the mouse just released,
/// fold the live delta into the persisted override map.
void commit_drag_release(DragState& drag) {
    if (!drag.active) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;

    auto& slot = drag.overrides[drag.target];
    slot.x += drag.live_delta.x;
    slot.y += drag.live_delta.y;

    drag.active     = false;
    drag.live_delta = ImVec2(0.0f, 0.0f);
}

/// @brief Render the Apply / Revert footer + a pending-count chip.
/// Returns true when the user clicked Apply or Revert (signals the
/// caller to refresh whatever it's caching).
void render_footer(DragState& drag) {
    const bool dirty = !drag.overrides.empty();

    ImGui::PushFont(theme::font::body_sm);
    const std::size_t count = drag.overrides.size();
    if (dirty) {
        ImGui::TextColored(theme::palette::amber,
                           "%zu unsaved layout change%s",
                           count, count == 1 ? "" : "s");
    } else {
        ImGui::TextColored(theme::palette::paper_muted,
                           "Drag a display to rearrange. Adjacency only "
                           "— display source routing isn't editable yet.");
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
        // Persistence is a follow-up — adjacency overrides will
        // ride a future LayoutRecord through the mesh CRDT. For
        // now Apply just clears the pending map; the visual state
        // is the source of truth until reload.
        drag.overrides.clear();
    }
    ImGui::SameLine();
    if (pill_button("Revert##layout-revert", PillVariant::Secondary)) {
        drag.overrides.clear();
    }
    if (!dirty) ImGui::EndDisabled();
}

/// @brief Soft vertical S-curve between two points.
void draw_bezier(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    const float mid_y = (a.y + b.y) * 0.5f;
    dl->AddBezierCubic(a,
                       ImVec2(a.x, mid_y),
                       ImVec2(b.x, mid_y),
                       b, col, 2.0f, 32);
}

/// @brief Draw one bezier per display that points to its owning PC.
void draw_identity_routes(ImDrawList* dl,
                          const std::vector<Node>& nodes) {
    for (const Node& sink : nodes) {
        if (sink.kind != Node::Kind::Display) continue;
        for (const Node& src : nodes) {
            if (src.kind != Node::Kind::Pc) continue;
            if (src.machine_id != sink.machine_id) continue;
            draw_bezier(dl,
                        ImVec2((src.tl.x + src.br.x) * 0.5f, src.br.y),
                        ImVec2((sink.tl.x + sink.br.x) * 0.5f, sink.tl.y),
                        ImGui::ColorConvertFloat4ToU32(machine_color(src.machine_id)));
            break;
        }
    }
}

/// @brief Return the unique machine ids in the order they first
/// appear in @p displays.
std::vector<std::string> unique_machines(
    const std::vector<orchestrator::Display>& displays) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& d : displays) {
        if (seen.insert(d.machine_id).second) {
            out.push_back(d.machine_id);
        }
    }
    return out;
}

}  // namespace

void render(orchestrator::IOrchestrator& orch) {
    static DragState drag;

    // The footer reserves a fixed-height strip at the bottom of
    // the available region. Computed up front so the bottom band
    // knows its draw bounds + the auto-fit scale stays inside the
    // visible canvas without needing a scroll.
    constexpr float kFooterHeight = 44.0f;
    constexpr float kFooterGap    = theme::space::sm;

    const ImVec2 avail   = ImGui::GetContentRegionAvail();
    const ImVec2 origin  = ImGui::GetCursorScreenPos();
    const float  canvas_h =
        std::max(180.0f, avail.y - kFooterHeight - kFooterGap);
    const ImVec2 end(origin.x + avail.x, origin.y + canvas_h);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, end, kCanvasBg, theme::radius::md);

    const auto displays = orch.displays();
    const auto machines = unique_machines(displays);

    std::vector<Node> nodes;
    nodes.reserve(machines.size() + displays.size());
    draw_top_band(dl, origin, avail.x, machines,
                  orch.local_machine_id(), nodes);
    draw_bottom_band(dl, origin, avail.x, canvas_h,
                     displays, nodes, drag);
    draw_identity_routes(dl, nodes);
    commit_drag_release(drag);

    // Reserve canvas space + gap in the ImGui layout cursor, then
    // pin the footer's Y position so it lands exactly at
    // (origin.y + canvas_h + gap) — anchored to the canvas, not
    // to the layout cursor's accumulated drift.
    ImGui::Dummy(ImVec2(avail.x, canvas_h));
    ImGui::SetCursorScreenPos(ImVec2(origin.x, end.y + kFooterGap));
    render_footer(drag);
}

}  // namespace unio_ui::ui::layout
