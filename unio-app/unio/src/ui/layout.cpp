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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace unio_ui::ui::layout {

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
                   DragState& drag,
                   bool identifying) {
    if (displays.empty()) return;

    // Per-peer X shift — disjoint columns regardless of each
    // peer's own coordinate-space origin.
    std::int32_t strip_w = 0;
    const auto peer_offset =
        compute_peer_render_offsets(displays, strip_w);

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

    const float strip_screen_w = data_w * scale;
    const float strip_screen_h = data_h * scale;
    const float pan_x = origin.x
                      + std::max(kStripSidePad,
                                  (width - strip_screen_w) * 0.5f);
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

        if (identifying) {
            // Identify mode — washes the rect with the machine's
            // accent and stamps the global number plus machine_id
            // big enough to read at a glance. Same intent as the
            // Python tree's per-monitor fullscreen overlay; the
            // physical-screen variant lands when a real control
            // channel can fan the trigger to remote peers.
            dl->AddRectFilled(
                ImVec2(sx, sy), ImVec2(sx + sw, sy + sh),
                ImGui::ColorConvertFloat4ToU32(blend(color, 0.85f)),
                theme::radius::sm);
            dl->AddRect(
                ImVec2(sx, sy), ImVec2(sx + sw, sy + sh),
                ImGui::ColorConvertFloat4ToU32(theme::palette::paper_text),
                theme::radius::sm, 0, 3.0f);

            char big_num[8];
            std::snprintf(big_num, sizeof(big_num), "%d", d.number);
            ImGui::PushFont(theme::font::title);
            const ImVec2 bs = ImGui::CalcTextSize(big_num);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        ImVec2(sx + (sw - bs.x) * 0.5f,
                               sy + (sh - bs.y) * 0.5f - bs.y * 0.4f),
                        ImGui::ColorConvertFloat4ToU32(theme::palette::paper_text),
                        big_num);
            ImGui::PopFont();
            const ImVec2 ls = ImGui::CalcTextSize(d.machine_id.c_str());
            dl->AddText(ImVec2(sx + (sw - ls.x) * 0.5f,
                               sy + sh * 0.78f),
                        ImGui::ColorConvertFloat4ToU32(theme::palette::paper_text),
                        d.machine_id.c_str());
        } else {
            // Default mode: small centred number + label below.
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
        }

        // Hit-test for click-down; helper guards against starting
        // a second drag while one is already active.
        try_start_drag(key, sx, sy, sw, sh, mouse, drag);
    }
}

}  // namespace

void render(orchestrator::IOrchestrator& orch) {
    static DragState drag;
    static std::chrono::steady_clock::time_point identify_until{};

    const bool identifying =
        std::chrono::steady_clock::now() < identify_until;

    // Reserve a fixed-height footer at the bottom so the auto-fit
    // strip stays inside the visible region without scrolling.
    constexpr float kFooterHeight = 44.0f;
    constexpr float kFooterGap    = theme::space::sm;

    const ImVec2 avail   = ImGui::GetContentRegionAvail();
    const ImVec2 origin  = ImGui::GetCursorScreenPos();
    const float  canvas_h =
        std::max(180.0f, avail.y - kFooterHeight - kFooterGap);
    const ImVec2 end(origin.x + avail.x, origin.y + canvas_h);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, end, kCanvasBg, theme::radius::md);

    draw_grid(dl, origin, avail.x, canvas_h);
    draw_displays(dl, origin, avail.x, canvas_h, orch.displays(),
                  drag, identifying);
    commit_drag_release(drag);

    // Reserve canvas region in the layout cursor + anchor the
    // footer immediately below it.
    ImGui::Dummy(ImVec2(avail.x, canvas_h));
    ImGui::SetCursorScreenPos(ImVec2(origin.x, end.y + kFooterGap));
    render_layout_footer(drag, identify_until);
}

}  // namespace unio_ui::ui::layout
