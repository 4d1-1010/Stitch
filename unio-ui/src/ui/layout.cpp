/// @file layout.cpp
/// @brief Two-row layout canvas: PC strip + display strip + identity routes.

#include "ui/layout.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

/// Hue slice centred on lilac, widened enough to give distinct
/// per-machine shades while keeping the palette family coherent.
constexpr float kLilacHue = 252.0f / 360.0f;
constexpr float kHueSpan  = 0.18f;
constexpr float kSatMin   = 0.55f;
constexpr float kSatMax   = 0.85f;
constexpr float kLightMin = 0.62f;
constexpr float kLightMax = 0.76f;

constexpr ImU32 kCanvasBg = IM_COL32(0xf8, 0xf9, 0xfc, 0xff);
constexpr ImU32 kGridLine = IM_COL32(0xed, 0xef, 0xf4, 0xff);

/// @brief CRC-32/IEEE on a byte string. Uses reflected polynomial
/// 0xEDB88320 with init 0xFFFFFFFF + final XOR.
std::uint32_t crc32(const std::string& s) {
    static std::array<std::uint32_t, 256> table = []() {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        crc = table[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/// @brief HLS (0..1) → RGB (0..1).
void hls_to_rgb(float h, float l, float s, float& r, float& g, float& b) {
    if (s == 0.0f) { r = g = b = l; return; }
    auto hue = [](float p, float q, float t) {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f / 2.0f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };
    const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    const float p = 2.0f * l - q;
    r = hue(p, q, h + 1.0f / 3.0f);
    g = hue(p, q, h);
    b = hue(p, q, h - 1.0f / 3.0f);
}

/// @brief Stable per-machine accent colour derived from the CRC-32
/// of the machine id.
ImVec4 machine_color(const std::string& machine_id) {
    const std::uint32_t seed = crc32(machine_id);
    const float h_part = ((seed >> 20) & 0xFFFu) / float(0xFFFu);
    const float s_part = ((seed >> 10) & 0x3FFu) / float(0x3FFu);
    const float l_part = ( seed        & 0x3FFu) / float(0x3FFu);
    const float h = std::fmod(kLilacHue + (h_part - 0.5f) * kHueSpan + 1.0f, 1.0f);
    const float s = kSatMin + s_part * (kSatMax - kSatMin);
    const float l = kLightMin + l_part * (kLightMax - kLightMin);
    float r, g, b;
    hls_to_rgb(h, l, s, r, g, b);
    return ImVec4(r, g, b, 1.0f);
}

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

    float min_x = displays[0].global_x;
    float max_x = displays[0].global_x + displays[0].width;
    for (const auto& d : displays) {
        if (d.global_x < min_x) min_x = d.global_x;
        if (d.global_x + d.width > max_x) max_x = d.global_x + d.width;
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
