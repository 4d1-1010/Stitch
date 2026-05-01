/// @file access.cpp
/// @brief Access tab — hardcoded-key gate.
///
/// Pre-launch placeholder. Single text input, masked, with an
/// Activate button. The orchestrator owns the comparison + the
/// authorized flag; this screen is render-only.

#include "ui/access.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/primitives.hpp"

#include <array>
#include <cstring>
#include <string>

namespace xorio::ui::access {

namespace {

/// @brief Offset the cursor so a @p width-pixel block sits centred
/// in the remaining content region.
void center_cursor_x(float width) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float pad = (avail - width) * 0.5f;
    if (pad > 0.0f) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
    }
}

/// @brief Per-frame input buffer. Resets across runs but persists
/// across frames within a single render cycle. Static-local so
/// the screen can be torn down + recreated without losing the
/// in-progress key while the user is mid-typing.
constexpr std::size_t kKeyBufferSize = 128;

/// @brief Pill-style chip rendered with the screen's accent palette.
void render_chip(const char* text, ImVec4 fill, ImVec4 fg) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const float  pad_x = 14.0f;
    const float  pad_y = 6.0f;
    center_cursor_x(sz.x + 2.0f * pad_x);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    const ImVec2 box_min = cur;
    const ImVec2 box_max(cur.x + sz.x + 2.0f * pad_x,
                         cur.y + sz.y + 2.0f * pad_y);
    dl->AddRectFilled(box_min, box_max,
                      ImGui::ColorConvertFloat4ToU32(fill),
                      (sz.y + 2.0f * pad_y) * 0.5f);

    ImGui::Dummy(ImVec2(sz.x + 2.0f * pad_x, sz.y + 2.0f * pad_y));
    const ImVec2 text_pos(box_min.x + pad_x, box_min.y + pad_y);
    dl->AddText(text_pos,
                ImGui::ColorConvertFloat4ToU32(fg),
                text);
}

}  // namespace

void render(orchestrator::IOrchestrator& orch) {
    constexpr float kTopOffset = 2.0f * 48.0f;
    constexpr float kInputWidth = 360.0f;

    // Input + last-attempt feedback survive across frames inside
    // this TU. They reset on process restart, which is the right
    // behaviour for a placeholder gate (no persistence yet).
    static std::array<char, kKeyBufferSize> key_buffer{};
    static bool   last_attempt_failed = false;
    static bool   focus_input         = true;

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + kTopOffset);

    // Title.
    ImGui::PushFont(theme::font::title);
    const float tw = ImGui::CalcTextSize("Access").x;
    center_cursor_x(tw);
    ImGui::TextColored(theme::palette::paper_text, "Access");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Spacing();

    if (orch.access_authorized()) {
        // Authorized branch: status chip + a one-line note.
        render_chip("Activated", theme::palette::lilac_soft,
                                 theme::palette::lilac);

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::PushFont(theme::font::body);
        const char* msg =
            "Access has been granted. Other tabs are unlocked.";
        const float mw = ImGui::CalcTextSize(msg).x;
        center_cursor_x(mw);
        ImGui::TextColored(theme::palette::paper_muted, "%s", msg);
        ImGui::PopFont();
        return;
    }

    // Unauthorized branch: copy + masked input + Activate button.
    ImGui::PushFont(theme::font::body);
    const char* prompt = "Enter your access key to unlock xorIO.";
    const float pw = ImGui::CalcTextSize(prompt).x;
    center_cursor_x(pw);
    ImGui::TextColored(theme::palette::paper_muted, "%s", prompt);
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    center_cursor_x(kInputWidth);
    ImGui::PushItemWidth(kInputWidth);
    if (focus_input) {
        ImGui::SetKeyboardFocusHere();
        focus_input = false;
    }
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_Password
                                    | ImGuiInputTextFlags_EnterReturnsTrue;
    const bool submitted_via_enter = ImGui::InputText(
        "##access_key", key_buffer.data(), key_buffer.size(), flags);
    ImGui::PopItemWidth();

    ImGui::Spacing();
    ImGui::Spacing();

    bool clicked = false;
    {
        // Approximate the pill-button width so we can centre the
        // single-button row. pill_button() handles its own
        // padding; we just need to position the cursor.
        const float btn_w = ImGui::CalcTextSize("Activate").x + 40.0f;
        center_cursor_x(btn_w);
        clicked = pill_button("Activate", PillVariant::Primary);
    }

    if (clicked || submitted_via_enter) {
        const std::string key(key_buffer.data());
        const bool ok = orch.try_authorize(key);
        last_attempt_failed = !ok;
        if (ok) {
            // Wipe the buffer so the masked digits don't linger
            // in the static if the user navigates back to this
            // tab after unlocking.
            key_buffer.fill(0);
        }
    }

    if (last_attempt_failed) {
        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::PushFont(theme::font::body_sm);
        const char* err = "Wrong key — try again.";
        const float ew = ImGui::CalcTextSize(err).x;
        center_cursor_x(ew);
        ImGui::TextColored(theme::palette::coral, "%s", err);
        ImGui::PopFont();
    }
}

}  // namespace xorio::ui::access
