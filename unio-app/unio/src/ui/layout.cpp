/// @file layout.cpp
/// @brief Two-row layout canvas: PC strip + display strip + identity routes.

#include "ui/layout.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "ui/machine_color.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace unio_ui::ui::layout {

namespace {

constexpr float kTopBandY     = 16.0f;
constexpr float kTopBandH     = 84.0f;
constexpr float kBottomBandY  = kTopBandY + kTopBandH + 32.0f;
constexpr float kPcNodeW      = 200.0f;
constexpr float kPcNodeH      = 56.0f;
constexpr float kNodeGap      = 28.0f;
constexpr float kBandPadX     = 40.0f;
constexpr float kBottomScale  = 0.12f;

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
/// 120-pixel dot-grid behind them.
void draw_bottom_band(ImDrawList* dl, const ImVec2& origin,
                      float width, float height,
                      const std::vector<orchestrator::Display>& displays,
                      std::vector<Node>& out_nodes) {
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

    float min_x = static_cast<float>(displays[0].global_x);
    float max_x = static_cast<float>(displays[0].global_x + displays[0].width);
    for (const auto& d : displays) {
        if (d.global_x < min_x) min_x = static_cast<float>(d.global_x);
        if (d.global_x + d.width > max_x) {
            max_x = static_cast<float>(d.global_x + d.width);
        }
    }
    const float strip_w = (max_x - min_x) * kBottomScale;
    const float pan_x = origin.x + std::max(20.0f, (width - strip_w) * 0.5f)
                        - min_x * kBottomScale;
    const float pan_y = grid_y0 - (displays[0].global_y * kBottomScale);

    for (const auto& d : displays) {
        const float sx = pan_x + d.global_x * kBottomScale;
        const float sy = pan_y + d.global_y * kBottomScale;
        const float sw = d.width  * kBottomScale;
        const float sh = d.height * kBottomScale;

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
    }
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
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 end(origin.x + avail.x, origin.y + avail.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, end, kCanvasBg, theme::radius::md);

    const auto displays = orch.displays();
    const auto machines = unique_machines(displays);

    std::vector<Node> nodes;
    nodes.reserve(machines.size() + displays.size());
    draw_top_band(dl, origin, avail.x, machines,
                  orch.local_machine_id(), nodes);
    draw_bottom_band(dl, origin, avail.x, avail.y, displays, nodes);
    draw_identity_routes(dl, nodes);

    ImGui::Dummy(avail);
}

}  // namespace unio_ui::ui::layout
