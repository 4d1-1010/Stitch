/// @file layout.cpp
/// @brief Layout tab — display-only canvas.
///
/// Scope: render the dot-grid backdrop + one rectangle per display
/// across every peer, in machine-coloured fills. Each peer's
/// displays sit in their own horizontal column so peers whose
/// own coordinate spaces overlap (every peer with a primary at
/// `(0, 0)`) don't collide visually. Drag interaction + Apply /
/// Revert footer live in `layout_drag.cpp`.

#include "ui/layout.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/layout_drag.hpp"
#include "ui/machine_color.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace xorio_ui::ui::layout {

namespace {

// ── Tunable constants ──────────────────────────────────────────

constexpr float kStripTopY      = 18.0f;
constexpr float kStripBottomPad = 10.0f;
constexpr float kStripSidePad   = 20.0f;

/// @brief Bounds on the auto-fit zoom (display-px → screen-px).
constexpr float kBottomScaleMax = 0.12f;
constexpr float kBottomScaleMin = 0.02f;

/// @brief Gap (in display-coordinate units) inserted between two
/// peer columns so their rectangles are visibly separate even at
/// the smallest zoom.
constexpr std::int32_t kInterPeerGap = 200;

constexpr ImU32 kCanvasBg = IM_COL32(0xf8, 0xf9, 0xfc, 0xff);
constexpr ImU32 kGridLine = IM_COL32(0xed, 0xef, 0xf4, 0xff);

// ── Helpers ────────────────────────────────────────────────────

/// @brief Blend @p fg over @p bg at alpha @p a, producing an
/// opaque colour. Used for the rectangle interior wash.
ImVec4 blend(ImVec4 fg, float a, ImVec4 bg = {0.973f, 0.977f, 0.988f, 1.0f}) {
    return ImVec4(fg.x * a + bg.x * (1.0f - a),
                  fg.y * a + bg.y * (1.0f - a),
                  fg.z * a + bg.z * (1.0f - a),
                  1.0f);
}

/// @brief Compute a per-peer X shift so each peer's displays sit
/// in their own horizontal column in the rendered strip.
///
/// The shift is in display-coordinate units; rendered_x for any
/// display @c d is `d.global_x + offsets[d.machine_id]`. After the
/// shift every peer's column starts at the previous peer's right
/// edge plus a small gap. Peers are visited in alphabetical order
/// so the layout is stable frame-to-frame.
struct PeerExtent { std::int32_t min_x = 0; std::int32_t max_x = 0; };

std::map<std::string, std::int32_t>
compute_peer_render_offsets(
    const std::vector<orchestrator::Display>& displays,
    std::int32_t& out_total_width) {
    std::map<std::string, PeerExtent> extents;
    for (const auto& d : displays) {
        auto [it, inserted] = extents.emplace(
            d.machine_id, PeerExtent{d.global_x, d.global_x + d.width});
        if (!inserted) {
            it->second.min_x = std::min(it->second.min_x, d.global_x);
            it->second.max_x = std::max(it->second.max_x,
                                        d.global_x + d.width);
        }
    }

    std::map<std::string, std::int32_t> offsets;
    std::int32_t cursor = 0;
    for (const auto& [mid, ext] : extents) {
        offsets[mid] = cursor - ext.min_x;
        cursor += (ext.max_x - ext.min_x) + kInterPeerGap;
    }
    if (!extents.empty()) cursor -= kInterPeerGap;  // trailing gap.
    out_total_width = cursor;
    return offsets;
}

// ── Display strip drawing ──────────────────────────────────────

void draw_grid(ImDrawList* dl, ImVec2 origin, float width, float height) {
    const float x0 = origin.x + kStripSidePad;
    const float y0 = origin.y + kStripTopY;
    const float x1 = origin.x + width  - kStripSidePad;
    const float y1 = origin.y + height - kStripBottomPad;

    constexpr float kCellSize = 120.0f;
    for (float gx = x0; gx < x1; gx += kCellSize) {
        dl->AddLine(ImVec2(gx, y0), ImVec2(gx, y1), kGridLine, 1.0f);
    }
    for (float gy = y0; gy < y1; gy += kCellSize) {
        dl->AddLine(ImVec2(x0, gy), ImVec2(x1, gy), kGridLine, 1.0f);
    }
}

void draw_displays(ImDrawList* dl, ImVec2 origin,
                   float width, float height,
                   const std::vector<orchestrator::Display>& displays,
                   const std::map<std::string, std::int32_t>& peer_offset,
                   DragState& drag) {
    if (displays.empty()) return;

    // Recompute strip_w from the (caller-supplied) offsets +
    // displays — width spans from min(d.x + offset) to
    // max(d.x + offset + d.width).
    std::int32_t strip_min = 0;
    std::int32_t strip_max = 0;
    {
        bool first = true;
        for (const auto& d : displays) {
            auto it = peer_offset.find(d.machine_id);
            const std::int32_t off = it != peer_offset.end() ? it->second : 0;
            const std::int32_t lo  = d.global_x + off;
            const std::int32_t hi  = lo + d.width;
            if (first) { strip_min = lo; strip_max = hi; first = false; }
            else {
                if (lo < strip_min) strip_min = lo;
                if (hi > strip_max) strip_max = hi;
            }
        }
    }
    const std::int32_t strip_w = strip_max - strip_min;

    // Y bounds collected after the X shift is applied (strip_w was
    // computed from the X axis only; Y stays in peer coords).
    float min_y = static_cast<float>(displays[0].global_y);
    float max_y = static_cast<float>(displays[0].global_y + displays[0].height);
    for (const auto& d : displays) {
        if (d.global_y < min_y) min_y = static_cast<float>(d.global_y);
        if (d.global_y + d.height > max_y) {
            max_y = static_cast<float>(d.global_y + d.height);
        }
    }
    const float data_w = std::max(1.0f, static_cast<float>(strip_w));
    const float data_h = std::max(1.0f, max_y - min_y);

    // Auto-fit isotropic scale, clamped.
    const float band_w = std::max(1.0f, width  - 2.0f * kStripSidePad);
    const float band_h = std::max(1.0f, height - kStripTopY - kStripBottomPad);
    float scale = std::min(kBottomScaleMax,
                           std::min(band_w / data_w, band_h / data_h));
    if (scale < kBottomScaleMin) scale = kBottomScaleMin;
    drag.last_scale = scale;

    const float strip_screen_w = data_w * scale;
    const float strip_screen_h = data_h * scale;
    const float pan_x = origin.x
                      + std::max(kStripSidePad,
                                  (width - strip_screen_w) * 0.5f)
                      - static_cast<float>(strip_min) * scale;
    const float pan_y = origin.y + kStripTopY
                      + std::max(0.0f, (band_h - strip_screen_h) * 0.5f)
                      - min_y * scale;

    // Mouse + drag update — the helper sees this same scale + pan
    // so its collision math matches what we draw.
    const ImVec2 mouse      = ImGui::GetMousePos();
    const bool   mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (drag.active && mouse_down) {
        update_drag_with_collision(displays, scale, pan_x, pan_y,
                                   mouse, peer_offset, drag);
    }

    for (const auto& d : displays) {
        const DisplayKey key{d.machine_id, d.monitor_id};

        const std::int32_t shifted_x = d.global_x + peer_offset.at(d.machine_id);
        const float sx_orig = pan_x + shifted_x  * scale;
        const float sy_orig = pan_y + d.global_y * scale;
        const float sw      = d.width  * scale;
        const float sh      = d.height * scale;

        // Saved override + (if dragging this rect) live delta.
        ImVec2 offset = saved_offset(drag, key);
        if (drag.active && drag.target == key) {
            offset.x += drag.live_delta.x;
            offset.y += drag.live_delta.y;
        }
        const float sx = sx_orig + offset.x;
        const float sy = sy_orig + offset.y;

        const ImVec4 color = machine_color(d.machine_id);
        const ImVec4 fill  = blend(color, 0.18f);

        dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh),
                          ImGui::ColorConvertFloat4ToU32(fill),
                          theme::radius::sm);
        dl->AddRect(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh),
                    ImGui::ColorConvertFloat4ToU32(color),
                    theme::radius::sm, 0, 3.0f);

        // Centred number; subtitle uses machine_id:monitor_id when
        // the rectangle is large enough to host two lines. The
        // Identify button surfaces a separate fullscreen overlay
        // on each physical monitor (see render_layout_footer);
        // the canvas itself stays unchanged.
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

        // Hit-test for click-down; helper guards against starting
        // a second drag while one is already active.
        try_start_drag(key, sx, sy, sw, sh, mouse, drag);
    }
}

}  // namespace

void render(orchestrator::IOrchestrator& orch) {
    static DragState  drag;
    static std::string selected_ws_id;

    const auto        workspaces_list = orch.workspaces();
    const std::string local_id        = orch.local_machine_id();

    // Re-validate the persisted selection: if the workspace was
    // deleted, or the local PC was removed from it, drop it.
    if (!selected_ws_id.empty()) {
        bool valid = false;
        for (const auto& ws : workspaces_list) {
            if (ws.id == selected_ws_id
                && ws.members.count(local_id) > 0) {
                valid = true;
                break;
            }
        }
        if (!valid) selected_ws_id.clear();
    }
    // Default to the first workspace the local PC belongs to.
    if (selected_ws_id.empty()) {
        for (const auto& ws : workspaces_list) {
            if (ws.members.count(local_id) > 0) {
                selected_ws_id = ws.id;
                break;
            }
        }
    }

    // ── Workspace selector dropdown ────────────────────────────
    if (workspaces_list.empty()) {
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_faint,
                           "No workspace accessible.");
        ImGui::PopFont();
    } else {
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_muted, "Workspace");
        ImGui::PopFont();
        ImGui::SameLine(0.0f, theme::space::sm);

        // Find the currently-selected workspace for the combo
        // preview label.
        const orchestrator::Workspace* sel = nullptr;
        for (const auto& ws : workspaces_list) {
            if (ws.id == selected_ws_id) { sel = &ws; break; }
        }
        const char* preview = sel ? sel->name.c_str() : "Select a workspace";

        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("##ws-sel", preview)) {
            for (const auto& ws : workspaces_list) {
                const bool is_member = ws.members.count(local_id) > 0;
                const bool is_selected = (ws.id == selected_ws_id);
                const std::string item =
                    ws.name + "##ws-item-" + ws.id;

                if (!is_member) ImGui::BeginDisabled();
                if (ImGui::Selectable(item.c_str(), is_selected)
                    && is_member) {
                    selected_ws_id = ws.id;
                }
                const bool hovered =
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
                if (!is_member) ImGui::EndDisabled();

                if (!is_member && hovered) {
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
                    ImGui::SetTooltip(
                        "Can't select — you're not in this workspace");
                    ImGui::PopStyleVar();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Dummy(ImVec2(0.0f, theme::space::sm));

    // ── Canvas ─────────────────────────────────────────────────
    constexpr float kFooterHeight = 44.0f;
    constexpr float kFooterGap    = theme::space::sm;

    const float  avail_x  = ImGui::GetContentRegionAvail().x;
    const float  avail_y  = ImGui::GetContentRegionAvail().y;
    const ImVec2 origin   = ImGui::GetCursorScreenPos();
    const float  canvas_h =
        std::max(180.0f, avail_y - kFooterHeight - kFooterGap);
    const ImVec2 end(origin.x + avail_x, origin.y + canvas_h);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, end, kCanvasBg, theme::radius::md);
    draw_grid(dl, origin, avail_x, canvas_h);

    // Filter displays to the selected workspace's members. With no
    // selectable workspace the canvas stays empty (bg + grid only)
    // and the footer disables itself — the explanatory text is
    // already up at the selector strip ("No workspace accessible.").
    std::vector<orchestrator::Display> displays;
    bool footer_disabled = false;
    const orchestrator::Workspace* sel = nullptr;

    if (workspaces_list.empty()) {
        footer_disabled = true;
    } else {
        if (!selected_ws_id.empty()) {
            for (const auto& ws : workspaces_list) {
                if (ws.id == selected_ws_id) { sel = &ws; break; }
            }
        }
        if (sel) {
            displays = orch.displays();
            std::vector<orchestrator::Display> filtered;
            filtered.reserve(displays.size());
            for (const auto& d : displays) {
                if (sel->members.count(d.machine_id) > 0) {
                    filtered.push_back(d);
                }
            }
            displays = std::move(filtered);
        } else {
            footer_disabled = true;
        }
    }

    // Compute per-peer strip offsets from the *raw probe* coords
    // (displays before any layout override). These offsets are
    // the single source of truth for the render path: they drive
    // both the layout-override pre-pass below AND the X shift
    // applied inside draw_displays. Recomputing them after the
    // override would feed back the saved values, undoing the
    // user's horizontal arrangement — that was the bug where
    // Apply's vertical changes survived but horizontal didn't.
    std::map<std::string, std::int32_t> peer_offset;
    if (!displays.empty()) {
        std::int32_t strip_w_unused = 0;
        peer_offset = compute_peer_render_offsets(displays, strip_w_unused);
    }

    // Apply workspace.layout on top of the raw probe coords. We
    // subtract the strip offset so that draw_displays adding the
    // same offset back yields the saved global position.
    if (sel && !sel->layout.empty() && !displays.empty()) {
        for (auto& d : displays) {
            for (const auto& e : sel->layout) {
                if (e.machine_id == d.machine_id
                    && e.monitor_id == d.monitor_id) {
                    auto it = peer_offset.find(d.machine_id);
                    const std::int32_t off =
                        it != peer_offset.end() ? it->second : 0;
                    d.global_x = e.global_x - off;
                    d.global_y = e.global_y;
                    break;
                }
            }
        }
    }

    draw_displays(dl, origin, avail_x, canvas_h, displays,
                   peer_offset, drag);
    commit_drag_release(drag);

    // Apply context — final rendered global position is
    // d.global_x + peer_offset (without the per-frame
    // recompute, so saved layout survives a round-trip).
    ApplyContext ctx;
    ctx.workspace_id = sel ? sel->id : "";
    ctx.scale        = drag.last_scale;
    ctx.displays.reserve(displays.size());
    for (const auto& d : displays) {
        orchestrator::Display rendered = d;
        auto it = peer_offset.find(d.machine_id);
        if (it != peer_offset.end()) rendered.global_x += it->second;
        ctx.displays.push_back(std::move(rendered));
    }

    // Reserve canvas region in the layout cursor + anchor the
    // footer immediately below it.
    ImGui::Dummy(ImVec2(avail_x, canvas_h));
    ImGui::SetCursorScreenPos(ImVec2(origin.x, end.y + kFooterGap));
    render_layout_footer(orch, drag, footer_disabled, ctx);
}

}  // namespace xorio_ui::ui::layout
