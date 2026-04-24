/*! @file activity.cpp
 *  @brief Activity-tab body.
 */

#include "activity.hpp"

#include "imgui.h"

#include "../theme.hpp"

namespace unio_ui::screens::activity {

namespace {

/// Horizontally centre the cursor so a widget of @p width renders
/// centred in the remaining content region.
void center_cursor_x(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float pad = (avail - width) * 0.5f;
    if (pad > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    }
}

/// Render "Welcome to unIO" with the three-colour split from
/// `_activity_alone_state` (PAPER_TEXT / LILAC / PAPER_MUTED),
/// at title size (Inter Bold 22 pt).
void render_title() {
    const char* prefix = "Welcome to ";
    const char* un     = "un";
    const char* io     = "IO";

    ImGui::PushFont(theme::font::title);

    const float w_prefix = ImGui::CalcTextSize(prefix).x;
    const float w_un     = ImGui::CalcTextSize(un).x;
    const float w_io     = ImGui::CalcTextSize(io).x;
    const float total    = w_prefix + w_un + w_io;

    center_cursor_x(total);

    ImGui::TextColored(theme::palette::paper_text,  "%s", prefix);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(theme::palette::lilac,       "%s", un);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(theme::palette::paper_muted, "%s", io);

    ImGui::PopFont();
}

/// Subtitle block — text wrapped at 520 px, centred. Matches the
/// `wraplength=520, justify="center"` tk.Label in the Python.
void render_subtitle() {
    constexpr float kWrapWidth = 520.0f;
    const char* body =
        "unIO transforms the way you use multiple computers.\n\n\n"
        "Launch unIO on another computer — they'll find each "
        "other automatically.";

    center_cursor_x(kWrapWidth);
    const float start_x = ImGui::GetCursorPosX();
    ImGui::PushTextWrapPos(start_x + kWrapWidth);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::paper_muted);
    ImGui::TextUnformatted(body);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

/// Status line below the subtitle.
/// Three-state progression matching the Python:
///   1. grace-period    → "Looking for an activated unIO…"
///   2. not signed in   → "Sign in (Access tab)…"
///   3. signed in       → "Searching for peers on your LAN…"
///
/// The real tri-state dispatch needs the orchestrator stub
/// (`feedback` signed-in + discovery grace). For now, show the
/// grace-period message so the screen has visible status text.
void render_status() {
    const char* status =
        "Looking for an activated unIO on your LAN…";

    ImGui::PushFont(theme::font::body_sm);
    const float w = ImGui::CalcTextSize(status).x;
    center_cursor_x(w);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::paper_muted);
    ImGui::TextUnformatted(status);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

}  // namespace

void render() {
    // Python: center_block.place(relx=0.5, y=2 * logo_h, anchor="n")
    // where logo_h = 48. The gap creates breathing room between
    // the top bar and the welcome message.
    constexpr float kTopOffset = 2.0f * 48.0f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + kTopOffset);

    render_title();
    ImGui::Spacing();
    ImGui::Spacing();

    render_subtitle();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    render_status();
}

}  // namespace unio_ui::screens::activity
