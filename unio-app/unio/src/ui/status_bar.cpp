/// @file status_bar.cpp
/// @brief Translation-unit-local state for the bottom status bar.
///
/// Single-message model: every `post()` replaces the in-flight
/// message. Auto-fades after `kDwell` so a stale message doesn't
/// linger, but the bar's row stays in the layout (renders empty)
/// to keep window heights stable.

#include "ui/status_bar.hpp"

#include "imgui.h"

#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"

#include <chrono>
#include <mutex>
#include <optional>

namespace unio_ui::ui::status {

namespace {

using Clock = std::chrono::steady_clock;

/// @brief How long a posted message stays visible before fading.
constexpr auto kDwell = std::chrono::seconds(4);

/// @brief Bar height in pixels. Stays constant so the layout
/// underneath doesn't reflow when a message arrives.
constexpr float kBarHeight = 28.0f;

struct Message {
    Level             level;
    std::string       text;
    Clock::time_point posted_at;
};

std::mutex             g_mutex;
std::optional<Message> g_current;

/// @brief (background, foreground) wash for a level — kept in this
/// TU because the bar's palette deliberately doesn't reuse the
/// inline-alert palette (different ergonomics: footer vs header).
struct Palette {
    ImVec4 bg;
    ImVec4 fg;
};

Palette wash_for(Level level) {
    switch (level) {
        case Level::Warn:
            return {theme::rgba_hex(0x2a1f24), theme::palette::coral};
        case Level::Info:
        default:
            return {theme::rgba_hex(0x1e1d24), theme::palette::lilac};
    }
}

/// @brief Snapshot the current message (held only briefly under
/// the mutex) and clear it if it has aged past the dwell.
std::optional<Message> snapshot_and_age() {
    std::lock_guard lk(g_mutex);
    if (!g_current) return std::nullopt;
    if (Clock::now() - g_current->posted_at > kDwell) {
        g_current.reset();
        return std::nullopt;
    }
    return g_current;
}

}  // namespace

void post(Level level, std::string text) {
    std::lock_guard lk(g_mutex);
    g_current = Message{level, std::move(text), Clock::now()};
}

void render() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 origin(vp->WorkPos.x,
                        vp->WorkPos.y + vp->WorkSize.y - kBarHeight);
    const ImVec2 br(vp->WorkPos.x + vp->WorkSize.x,
                    vp->WorkPos.y + vp->WorkSize.y);

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    const auto msg = snapshot_and_age();
    const ImVec4 bg = msg ? wash_for(msg->level).bg
                          : theme::palette::paper_rail_deep;
    dl->AddRectFilled(origin, br,
                      ImGui::ColorConvertFloat4ToU32(bg));

    if (!msg) return;

    const Palette p = wash_for(msg->level);

    // Render text vertically centred + left-padded.
    ImGui::PushFont(theme::font::body_sm);
    const ImVec2 text_size = ImGui::CalcTextSize(msg->text.c_str());
    const ImVec2 text_pos(origin.x + theme::space::lg,
                          origin.y + (kBarHeight - text_size.y) * 0.5f);
    dl->AddText(text_pos,
                ImGui::ColorConvertFloat4ToU32(p.fg),
                msg->text.c_str());
    ImGui::PopFont();
}

}  // namespace unio_ui::ui::status
