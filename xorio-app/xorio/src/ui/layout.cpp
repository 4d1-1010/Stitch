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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace xorio::ui::layout {

namespace {

// ── Tunable constants ──────────────────────────────────────────

/// @brief Single uniform inset between the canvas edge and the
/// grid / display strip on every side. Top, bottom, left, right
/// match so the strip reads as a centred box inside the canvas.
constexpr float kStripPad       = 16.0f;
// Legacy aliases — preserved so call sites already taking these
// names compile unchanged. Each resolves to the same kStripPad.
constexpr float kStripTopY      = kStripPad;
constexpr float kStripBottomPad = kStripPad;
constexpr float kStripSidePad   = kStripPad;

/// @brief Target on-screen grid-cell size in pixels. Actual
/// cell size is rounded so an integer count of cells fills the
/// strip exactly — every cell is the same size, no partial cell
/// at the right or bottom edge.
constexpr float kGridCellTarget = 100.0f;

// (Auto-fit used to clamp into [0.02, 0.12] but that meant the
// "Fit" button couldn't actually fit very small or very large
// display arrangements. The fit pass below now computes the
// exact scale that maps the data extents into the canvas
// extents, with no floor or ceiling — the user-zoom multiplier
// has its own bounds via kUserZoomMin / kUserZoomMax.)

/// @brief Bounds on the user-applied zoom multiplier — applied
/// on top of the auto-fit scale, so the effective scale ranges
/// from kBottomScaleMin*kUserZoomMin to kBottomScaleMax*
/// kUserZoomMax. Wide enough to inspect a single display
/// pixel-for-pixel while still allowing a far-out overview.
constexpr float kUserZoomMin = 0.25f;
constexpr float kUserZoomMax = 16.0f;
constexpr float kUserZoomStep = 1.15f;

/// @brief Persistent user-driven view state. Layered on top of
/// the auto-fit so zooming back to defaults is just a matter of
/// resetting both fields. `user_scale` is a multiplier on the
/// auto-fit scale; `user_pan_dx/dy` is an additive screen-space
/// offset on top of the auto-fit pan.
struct CanvasView {
    float user_scale  = 1.0f;
    float user_pan_dx = 0.0f;
    float user_pan_dy = 0.0f;
    bool  panning     = false;
    /// @brief Button-driven zoom request applied on the next
    /// canvas render with the canvas centre as the anchor.
    /// Buttons are rendered ABOVE the canvas so they don't yet
    /// know the canvas geometry; deferring the action a frame
    /// keeps centre-zoom math correct without forward-leaking
    /// geometry up the layout cursor.
    float pending_zoom_factor = 1.0f;
    bool  pending_fit         = false;
};

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
    const float x0 = origin.x + kStripPad;
    const float y0 = origin.y + kStripPad;
    const float x1 = origin.x + width  - kStripPad;
    const float y1 = origin.y + height - kStripPad;
    const float grid_w = x1 - x0;
    const float grid_h = y1 - y0;
    if (grid_w <= 1.0f || grid_h <= 1.0f) return;

    // Pick column / row counts that give cells closest to the
    // target size — division then yields exact, equal cells with
    // no partial leftover at the right / bottom edges.
    const int n_cols = std::max(
        1, static_cast<int>(std::lround(grid_w / kGridCellTarget)));
    const int n_rows = std::max(
        1, static_cast<int>(std::lround(grid_h / kGridCellTarget)));
    const float cell_w = grid_w / n_cols;
    const float cell_h = grid_h / n_rows;

    // Vertical lines — one at every column boundary including
    // the closing line at x1 (i = 0 .. n_cols inclusive).
    for (int i = 0; i <= n_cols; ++i) {
        const float gx = x0 + cell_w * i;
        dl->AddLine(ImVec2(gx, y0), ImVec2(gx, y1), kGridLine, 1.0f);
    }
    for (int j = 0; j <= n_rows; ++j) {
        const float gy = y0 + cell_h * j;
        dl->AddLine(ImVec2(x0, gy), ImVec2(x1, gy), kGridLine, 1.0f);
    }
}

/// @brief Result of the auto-fit pass — the scale + pan that
/// would centre the strip in the canvas at default zoom. The
/// user view (zoom + pan on top) is applied separately so
/// resetting the user view always lands back at this state.
struct AutoFit {
    float scale = 1.0f;
    float pan_x = 0.0f;
    float pan_y = 0.0f;
    bool  empty = true;  ///< true when displays.empty() — caller skips draw.
};

AutoFit compute_auto_fit(
    const std::vector<orchestrator::Display>& displays,
    const std::map<std::string, std::int32_t>& peer_offset,
    ImVec2 origin, float width, float height) {
    AutoFit r;
    if (displays.empty()) return r;
    r.empty = false;

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
    const float band_w = std::max(1.0f, width  - 2.0f * kStripSidePad);
    const float band_h = std::max(1.0f, height - kStripTopY - kStripBottomPad);
    // Exact fit — pick the tighter of the two axis ratios so
    // every display lands inside the canvas band on both axes.
    // No clamp: Fit must always show everything, regardless of
    // how small or large the data arrangement is.
    r.scale = std::min(band_w / data_w, band_h / data_h);

    const float strip_screen_w = data_w * r.scale;
    const float strip_screen_h = data_h * r.scale;
    r.pan_x = origin.x
            + std::max(kStripSidePad,
                        (width - strip_screen_w) * 0.5f)
            - static_cast<float>(strip_min) * r.scale;
    r.pan_y = origin.y + kStripTopY
            + std::max(0.0f, (band_h - strip_screen_h) * 0.5f)
            - min_y * r.scale;
    return r;
}

void draw_displays(ImDrawList* dl,
                   ImVec2 canvas_min, ImVec2 canvas_max,
                   float scale, float pan_x, float pan_y,
                   float text_scale,
                   const std::vector<orchestrator::Display>& displays,
                   const std::map<std::string, std::int32_t>& peer_offset,
                   DragState& drag) {
    if (displays.empty()) return;
    drag.last_scale = scale;

    // Scale the text proportional to the user's zoom so labels
    // shrink when zoomed out (don't crowd small rectangles) and
    // grow when zoomed in (stay readable on a single magnified
    // monitor). Clamped at the low end so labels never become
    // sub-pixel; at the high end so a deep zoom doesn't blow
    // text out beyond the rectangle.
    ImFont*     font     = ImGui::GetFont();
    const float base_pt  = ImGui::GetFontSize();
    const float font_pt  = std::clamp(base_pt * text_scale,
                                       base_pt * 0.6f,
                                       base_pt * 3.5f);

    // Mouse + drag update — the helper sees this same scale + pan
    // so its collision math matches what we draw, and gets the
    // canvas bounds so the rect can't escape the canvas even
    // when the cursor leaves the app window.
    const ImVec2 mouse      = ImGui::GetMousePos();
    const bool   mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (drag.active && mouse_down) {
        update_drag_with_collision(displays, scale, pan_x, pan_y,
                                   canvas_min, canvas_max,
                                   mouse, peer_offset, drag);
    }

    for (const auto& d : displays) {
        const DisplayKey key{d.machine_id, d.monitor_id};

        const std::int32_t shifted_x = d.global_x + peer_offset.at(d.machine_id);
        const float sx_orig = pan_x + shifted_x  * scale;
        const float sy_orig = pan_y + d.global_y * scale;
        const float sw      = d.width  * scale;
        const float sh      = d.height * scale;

        // Saved override is in display units → multiply by the
        // current scale to get a screen-pixel delta. live_delta
        // is already in screen pixels (mouse-tracking) and gets
        // folded into the override in display units on release.
        const ImVec2 offset_disp = saved_offset(drag, key);
        float offset_screen_x = offset_disp.x * scale;
        float offset_screen_y = offset_disp.y * scale;
        if (drag.active && drag.target == key) {
            offset_screen_x += drag.live_delta.x;
            offset_screen_y += drag.live_delta.y;
        }
        float sx = sx_orig + offset_screen_x;
        float sy = sy_orig + offset_screen_y;
        // Clamp the dragged rect's drawn position to the canvas
        // so the cursor going off the app window doesn't pull the
        // rect off-screen. live_delta itself is unclamped — the
        // auto-fit gradually extends instead.
        if (drag.active && drag.target == key) {
            const float min_x = canvas_min.x;
            const float max_x = std::max(canvas_min.x, canvas_max.x - sw);
            const float min_y = canvas_min.y;
            const float max_y = std::max(canvas_min.y, canvas_max.y - sh);
            sx = std::clamp(sx, min_x, max_x);
            sy = std::clamp(sy, min_y, max_y);
        }

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
        // Per-rect font fit: shrink the natural font_pt down so
        // the number always sits inside the rectangle. We
        // measure the glyph at font_pt then scale by the
        // tightest of (rect width / glyph width, rect height /
        // glyph height) capped at 1.0. Below kMinReadablePt we
        // skip drawing entirely — sub-6 px text is unreadable
        // and just clutters tiny rects on a deep zoom-out.
        constexpr float kMinReadablePt    = 6.0f;
        constexpr float kFitMargin        = 0.85f;  // 15% padding inside rect.
        const ImVec2 ns_natural =
            font->CalcTextSizeA(font_pt, FLT_MAX, 0.0f, num);
        const float fit_w_ratio = ns_natural.x > 0.0f
            ? sw * kFitMargin / ns_natural.x : 1.0f;
        const float fit_h_ratio = ns_natural.y > 0.0f
            ? sh * kFitMargin / ns_natural.y : 1.0f;
        const float num_pt =
            font_pt * std::min({1.0f, fit_w_ratio, fit_h_ratio});
        if (num_pt >= kMinReadablePt) {
            const ImVec2 ns =
                font->CalcTextSizeA(num_pt, FLT_MAX, 0.0f, num);
            dl->AddText(font, num_pt,
                        ImVec2(sx + (sw - ns.x) * 0.5f,
                               sy + (sh - ns.y) * 0.5f - num_pt * 0.4f),
                        ImGui::ColorConvertFloat4ToU32(theme::palette::paper_text),
                        num);
        }
        // Subtitle: same fit-to-rect treatment, plus a separate
        // gate that hides it when there isn't room for the
        // number AND a second line. Sub-pixel-readable subtitles
        // get suppressed entirely.
        const std::string label = d.machine_id + ":" + d.monitor_id;
        const ImVec2 ls_natural =
            font->CalcTextSizeA(font_pt, FLT_MAX, 0.0f, label.c_str());
        const float sub_fit_w = ls_natural.x > 0.0f
            ? sw * kFitMargin / ls_natural.x : 1.0f;
        const float sub_fit_h = ls_natural.y > 0.0f
            ? (sh - num_pt - 6.0f) * kFitMargin / ls_natural.y : 1.0f;
        const float sub_pt =
            font_pt * std::min({1.0f, sub_fit_w, sub_fit_h});
        if (num_pt >= kMinReadablePt
            && sub_pt >= kMinReadablePt
            && sh > num_pt + sub_pt + 8.0f) {
            const ImVec2 ls =
                font->CalcTextSizeA(sub_pt, FLT_MAX, 0.0f, label.c_str());
            dl->AddText(font, sub_pt,
                        ImVec2(sx + (sw - ls.x) * 0.5f,
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
    static DragState   drag;
    static CanvasView  view;
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

    // ── Zoom toolbar — right-aligned on the same row ──────────
    //
    // Order is "+ Fit -" per the user's preference. Lilac
    // (PillVariant::Primary) so the row reads as the canvas's
    // controls, separate from the secondary-styled buttons in
    // the footer. Buttons set a pending action on the view —
    // the canvas-render block below applies it once it knows
    // the canvas geometry (centre-zoom anchor).
    {
        // Approximate widths so we can reserve the right margin.
        const float plus_w = ImGui::CalcTextSize("+").x
                           + 2.0f * theme::space::lg;
        const float fit_w  = ImGui::CalcTextSize("Fit").x
                           + 2.0f * theme::space::lg;
        const float minus_w = ImGui::CalcTextSize("-").x
                            + 2.0f * theme::space::lg;
        const float gap = theme::space::sm;
        const float row_w = plus_w + fit_w + minus_w + 2.0f * gap;
        ImGui::SameLine();
        ImGui::SetCursorPosX(
            ImGui::GetCursorPosX()
            + ImGui::GetContentRegionAvail().x - row_w);
        // Render the toolbar at the disabled-Apply dimming level
        // so it reads as a secondary control (canvas chrome)
        // rather than a primary action like Apply / Revert. The
        // alpha multiplier is only visual — clicks still
        // register normally.
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().DisabledAlpha);
        if (pill_button("+##canvas-zoom-in", PillVariant::Primary)) {
            view.pending_zoom_factor *= kUserZoomStep;
        }
        ImGui::SameLine(0.0f, gap);
        if (pill_button("Fit##canvas-fit", PillVariant::Primary)) {
            view.pending_fit = true;
        }
        ImGui::SameLine(0.0f, gap);
        if (pill_button("-##canvas-zoom-out", PillVariant::Primary)) {
            view.pending_zoom_factor /= kUserZoomStep;
        }
        ImGui::PopStyleVar();
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

    // ── Auto-fit + user view (zoom + pan) ─────────────────────
    //
    // Auto-fit gives us a baseline scale + pan that centres the
    // strip in the canvas. User wheel zoom + middle-mouse pan
    // are layered on top via `view.user_scale` / `user_pan_dx`
    // / `user_pan_dy`, so a Reset View (button below) is just
    // those three fields back to defaults.
    //
    // Build a `fit_displays` copy that includes any uncommitted
    // drag deltas in display-coordinate space, converted via
    // the previous frame's scale (drag offsets are stored in
    // screen-pixel units). This is what we feed compute_auto_fit
    // so Fit / wheel-zoom-anchor reflect the in-progress
    // arrangement, not just the saved layout. The original
    // `displays` vector stays untouched: draw_displays applies
    // the same screen-space deltas on top of the rendered base,
    // and double-counting them via `fit_displays` here would
    // shift rectangles twice.
    // fit_displays includes committed overrides AND the in-
    // progress live_delta — so dragging a display toward a
    // canvas edge gradually extends the auto-fit (zooms out
    // just enough to keep the new position visible). Two things
    // keep this from running away the way it did earlier:
    //  - Canvas containment bounds live_delta (the rect can't
    //    leave the canvas, so live_delta plateaus once you
    //    reach the edge).
    //  - scale_start (captured on drag start) is the divisor for
    //    converting screen-pixel live_delta to display units;
    //    using the live `last_scale` instead created an
    //    exponential feedback (each pixel shrunk scale, each
    //    smaller scale inflated the next pixel's delta).
    std::vector<orchestrator::Display> fit_displays = displays;
    for (auto& d : fit_displays) {
        const DisplayKey k{d.machine_id, d.monitor_id};
        const auto it = drag.overrides.find(k);
        if (it != drag.overrides.end()) {
            d.global_x += static_cast<std::int32_t>(it->second.x);
            d.global_y += static_cast<std::int32_t>(it->second.y);
        }
    }
    if (drag.active && drag.scale_start > 0.0f) {
        const float inv = 1.0f / drag.scale_start;
        for (auto& d : fit_displays) {
            const DisplayKey k{d.machine_id, d.monitor_id};
            if (drag.target == k) {
                d.global_x += static_cast<std::int32_t>(
                    drag.live_delta.x * inv);
                d.global_y += static_cast<std::int32_t>(
                    drag.live_delta.y * inv);
                break;
            }
        }
    }

    AutoFit fit = compute_auto_fit(fit_displays, peer_offset,
                                    origin, avail_x, canvas_h);

    // Smooth BOTH fit-scale AND pan changes with a lerp toward
    // the auto-fit target. Lerp is engaged the moment a drag
    // starts and stays engaged AFTER mouse-release until the
    // smoothed values converge with the target — without the
    // post-release tail, fit.scale and pan would snap to the
    // un-smoothed target on release, the override (still in
    // screen-pixel units at the previous scale) wouldn't keep
    // up, and the rect would visibly drift in the direction
    // the user just dragged. Out-of-drag fits started by a
    // workspace switch or button-driven Fit bypass the lerp so
    // they still land instantly (no convergence in flight).
    static float prev_fit_scale  = 0.0f;
    static float prev_fit_pan_x  = 0.0f;
    static float prev_fit_pan_y  = 0.0f;
    static bool  smoothing_active = false;
    if (drag.active) smoothing_active = true;
    const float target_scale = fit.scale;
    const float target_pan_x = fit.pan_x;
    const float target_pan_y = fit.pan_y;
    if (smoothing_active && prev_fit_scale > 0.0f && fit.scale > 0.0f) {
        // Lerp toward target — with stable scale_start in
        // fit_displays the target itself moves linearly with
        // cursor, so a moderate damp tracks closely without the
        // jarring per-frame snaps of damp=1 or the multi-second
        // tail of the original 0.003.
        constexpr float kDampPerFrame = 0.35f;
        fit.scale = prev_fit_scale
            + (fit.scale - prev_fit_scale) * kDampPerFrame;
        // Once we override scale, recompute pan from the same
        // formula compute_auto_fit uses so the strip stays
        // centred at the new (smoothed) scale.
        const float band_h_pan = std::max(1.0f,
            canvas_h - 2.0f * kStripPad);
        std::int32_t strip_min = 0, strip_max = 0;
        float min_y = 0.0f, max_y = 0.0f;
        bool first = true;
        for (const auto& d : fit_displays) {
            const auto it = peer_offset.find(d.machine_id);
            const std::int32_t off = it != peer_offset.end()
                ? it->second : 0;
            const std::int32_t lo_x = d.global_x + off;
            const std::int32_t hi_x = lo_x + d.width;
            if (first) {
                strip_min = lo_x; strip_max = hi_x;
                min_y = static_cast<float>(d.global_y);
                max_y = static_cast<float>(d.global_y + d.height);
                first = false;
            } else {
                if (lo_x < strip_min) strip_min = lo_x;
                if (hi_x > strip_max) strip_max = hi_x;
                if (d.global_y < min_y)
                    min_y = static_cast<float>(d.global_y);
                if (d.global_y + d.height > max_y)
                    max_y = static_cast<float>(d.global_y + d.height);
            }
        }
        if (!first) {
            const float strip_screen_w =
                static_cast<float>(strip_max - strip_min) * fit.scale;
            const float strip_screen_h = (max_y - min_y) * fit.scale;
            // Recompute pan at the lerped scale — same formula
            // compute_auto_fit uses, so the strip stays centred
            // at the smoothed scale (different from `target_pan_*`
            // above which were captured at the un-smoothed
            // target scale).
            const float pan_x_for_smoothed_scale = origin.x
                + std::max(kStripPad,
                            (avail_x - strip_screen_w) * 0.5f)
                - static_cast<float>(strip_min) * fit.scale;
            const float pan_y_for_smoothed_scale = origin.y + kStripPad
                + std::max(0.0f, (band_h_pan - strip_screen_h) * 0.5f)
                - min_y * fit.scale;
            // Same lerp as scale — pan changes during drag
            // (recentre when the strip extents grow) ramp in
            // gradually so the rest of the canvas doesn't slide
            // out from under the user as they drag.
            fit.pan_x = prev_fit_pan_x
                + (pan_x_for_smoothed_scale - prev_fit_pan_x) * kDampPerFrame;
            fit.pan_y = prev_fit_pan_y
                + (pan_y_for_smoothed_scale - prev_fit_pan_y) * kDampPerFrame;
        }
    }
    prev_fit_scale = fit.scale;
    prev_fit_pan_x = fit.pan_x;
    prev_fit_pan_y = fit.pan_y;

    // Disengage smoothing once the lerp has converged with the
    // target. After this the next non-drag change (workspace
    // switch, hot-plug, button-driven Fit) snaps instantly.
    if (smoothing_active && !drag.active) {
        const float scale_eps =
            target_scale > 0.0f
                ? std::abs(fit.scale - target_scale) / target_scale
                : 0.0f;
        const float pan_eps = std::max(
            std::abs(fit.pan_x - target_pan_x),
            std::abs(fit.pan_y - target_pan_y));
        if (scale_eps < 0.001f && pan_eps < 0.5f) {
            smoothing_active = false;
        }
    }

    // Cursor must be inside the canvas rect to consume wheel /
    // pan input — otherwise a scroll over the workspace combo
    // (or a middle-click anywhere in the tab) would zoom the
    // canvas. ImGui::IsMouseHoveringRect uses screen coords.
    const bool canvas_hovered =
        ImGui::IsMouseHoveringRect(origin, end);
    const ImVec2 mouse_pos = ImGui::GetMousePos();

    // Mouse-wheel zoom, anchored on the cursor — the world
    // point under the mouse stays put after the scale change.
    // Skipped when the user is mid-rectangle-drag so a stray
    // wheel tick doesn't yank the canvas out from under them.
    if (canvas_hovered && !drag.active && !fit.empty) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const float factor = wheel > 0
                ? kUserZoomStep
                : 1.0f / kUserZoomStep;
            const float new_scale = std::clamp(view.user_scale * factor,
                                                kUserZoomMin,
                                                kUserZoomMax);
            const float real_factor = new_scale / view.user_scale;
            // Pan correction so the cursor world-point is stable.
            // pan_old = fit.pan + user_pan; pan_new must satisfy
            // pan_new = mouse - (mouse - pan_old) * real_factor.
            // Solve for user_pan_new:
            const float pan_old_x = fit.pan_x + view.user_pan_dx;
            const float pan_old_y = fit.pan_y + view.user_pan_dy;
            const float pan_new_x = mouse_pos.x
                - (mouse_pos.x - pan_old_x) * real_factor;
            const float pan_new_y = mouse_pos.y
                - (mouse_pos.y - pan_old_y) * real_factor;
            view.user_scale  = new_scale;
            view.user_pan_dx = pan_new_x - fit.pan_x;
            view.user_pan_dy = pan_new_y - fit.pan_y;
        }
    }

    // Pan via middle / right / left mouse — left only when the
    // click landed on empty canvas (not on a rectangle). The
    // left-button check has to wait until after draw_displays
    // (which calls try_start_drag and sets drag.active if the
    // click hit a rect); middle/right have no such conflict so
    // we initiate them up front here. Per-frame delta is
    // accumulated straight into user_pan_d* — more reliable
    // across ImGui versions than GetMouseDragDelta +
    // ResetMouseDragDelta, which can return zero on the first
    // frame after click and miss small movements depending on
    // the lock threshold.
    static ImVec2 pan_last{0.0f, 0.0f};
    static int    pan_button = -1;  // -1 idle; 0 left; 1 right; 2 middle.
    if (canvas_hovered && pan_button < 0) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            pan_button = ImGuiMouseButton_Middle;
            pan_last   = mouse_pos;
            view.panning = true;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            pan_button = ImGuiMouseButton_Right;
            pan_last   = mouse_pos;
            view.panning = true;
        }
    }

    // Effective scale + pan handed to the drawer + drag helper.
    const float eff_scale = fit.scale * view.user_scale;
    const float eff_pan_x = fit.pan_x + view.user_pan_dx;
    const float eff_pan_y = fit.pan_y + view.user_pan_dy;

    // Snapshot drag.active before draw — if the click this frame
    // ended up on a rectangle, draw_displays' try_start_drag
    // flips drag.active to true. We use that to distinguish
    // empty-canvas clicks (intent: pan) from rectangle clicks
    // (intent: drag-to-rearrange).
    const bool drag_was_active = drag.active;

    // Clip the display draws to the canvas rect so a deep zoom
    // or pan can't bleed rectangles past the canvas edges into
    // the workspace selector row or the footer below. Inset by
    // 1 px so the rounded canvas border (drawn separately) stays
    // visible — without the inset the clip rect lands on the
    // border edge and the AddRect outline pixel gets clipped.
    dl->PushClipRect(ImVec2(origin.x + 1.0f, origin.y + 1.0f),
                      ImVec2(end.x    - 1.0f, end.y    - 1.0f),
                      /*intersect_with_current=*/true);
    draw_displays(dl, /*canvas_min=*/origin, /*canvas_max=*/end,
                   eff_scale, eff_pan_x, eff_pan_y,
                   /*text_scale=*/view.user_scale,
                   displays, peer_offset, drag);
    dl->PopClipRect();
    commit_drag_release(drag);

    // Left-click pan — empty-canvas only. Engaged when the
    // click frame's hit-test on rectangles came back empty
    // (drag wasn't active before draw, isn't active after).
    // Without this gate a left-click on a rectangle would both
    // start the rectangle drag AND start a canvas pan.
    if (canvas_hovered && pan_button < 0
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !drag_was_active && !drag.active) {
        pan_button   = ImGuiMouseButton_Left;
        pan_last     = mouse_pos;
        view.panning = true;
    }
    // Per-frame pan-update for whichever button is active.
    if (view.panning) {
        if (ImGui::IsMouseDown(pan_button)) {
            view.user_pan_dx += mouse_pos.x - pan_last.x;
            view.user_pan_dy += mouse_pos.y - pan_last.y;
            pan_last = mouse_pos;
        } else {
            view.panning = false;
            pan_button   = -1;
        }
    }

    // Apply button-driven zoom now that we know the fit values
    // + canvas centre. Pending state is set by the toolbar
    // buttons above the canvas (rendered earlier in this
    // function); consumed here so the centre-zoom math reads
    // the up-to-date `fit`.
    if (!fit.empty
        && (view.pending_fit || view.pending_zoom_factor != 1.0f)) {
        if (view.pending_fit) {
            view.user_scale  = 1.0f;
            view.user_pan_dx = 0.0f;
            view.user_pan_dy = 0.0f;
        } else {
            const ImVec2 canvas_centre((origin.x + end.x) * 0.5f,
                                        (origin.y + end.y) * 0.5f);
            const float new_scale = std::clamp(
                view.user_scale * view.pending_zoom_factor,
                kUserZoomMin, kUserZoomMax);
            const float real_factor = new_scale / view.user_scale;
            const float pan_old_x = fit.pan_x + view.user_pan_dx;
            const float pan_old_y = fit.pan_y + view.user_pan_dy;
            const float pan_new_x = canvas_centre.x
                - (canvas_centre.x - pan_old_x) * real_factor;
            const float pan_new_y = canvas_centre.y
                - (canvas_centre.y - pan_old_y) * real_factor;
            view.user_scale  = new_scale;
            view.user_pan_dx = pan_new_x - fit.pan_x;
            view.user_pan_dy = pan_new_y - fit.pan_y;
        }
        view.pending_fit         = false;
        view.pending_zoom_factor = 1.0f;
    }

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

}  // namespace xorio::ui::layout
