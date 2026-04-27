/// @file activity.cpp
/// @brief Activity tab: welcome hero + peer list.

#include "ui/activity.hpp"

#include "imgui.h"

#include "orchestrator/orchestrator.hpp"
#include "theme/metrics.hpp"
#include "theme/palette.hpp"
#include "theme/typography.hpp"
#include "ui/activity_workspace_form.hpp"
#include "ui/activity_workspaces.hpp"
#include "ui/machine_color.hpp"
#include "ui/primitives.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace unio_ui::ui::activity {

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

/// @brief Render the three-colour "Welcome to unIO" heading.
void render_title() {
    const char* prefix = "Welcome to ";
    const char* un     = "un";
    const char* io     = "IO";

    ImGui::PushFont(theme::font::title);
    const float total = ImGui::CalcTextSize(prefix).x
                      + ImGui::CalcTextSize(un).x
                      + ImGui::CalcTextSize(io).x;
    center_cursor_x(total);

    ImGui::TextColored(theme::palette::paper_text,  "%s", prefix);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(theme::palette::lilac,       "%s", un);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::TextColored(theme::palette::paper_muted, "%s", io);
    ImGui::PopFont();
}

/// @brief Word-wrap @p text to @p wrap_width and centre each line.
/// Explicit `\n` characters break paragraphs; blank lines preserve
/// vertical rhythm.
void render_wrapped_centered(const char* text, float wrap_width,
                             ImVec4 color) {
    ImFont* font = ImGui::GetFont();
    const float font_size = font->LegacySize;
    const char* const text_end = text + std::strlen(text);
    const char* p = text;
    while (p <= text_end) {
        const char* para_end = p;
        while (para_end < text_end && *para_end != '\n') ++para_end;
        if (p == para_end) {
            ImGui::Dummy(ImVec2(1.0f, ImGui::GetTextLineHeight()));
        } else {
            const char* line = p;
            while (line < para_end) {
                const char* line_end = font->CalcWordWrapPosition(
                    font_size, line, para_end, wrap_width);
                if (line_end == line) line_end = line + 1;
                const ImVec2 sz = font->CalcTextSizeA(
                    font_size, FLT_MAX, 0.0f, line, line_end);
                center_cursor_x(sz.x);
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextUnformatted(line, line_end);
                ImGui::PopStyleColor();
                line = line_end;
                while (line < para_end && *line == ' ') ++line;
            }
        }
        if (para_end == text_end) break;
        p = para_end + 1;
    }
}

void render_subtitle() {
    constexpr float kWrapWidth = 520.0f;
    const char* body =
        "unIO transforms the way you use multiple computers.\n\n\n"
        "Launch unIO on another computer — they'll find each "
        "other automatically.";
    render_wrapped_centered(body, kWrapWidth, theme::palette::paper_muted);
}

/// @brief Pick the status line based on the current auth state.
void render_status(orchestrator::IOrchestrator& orch) {
    const char* status = nullptr;
    switch (orch.auth_state()) {
        case orchestrator::AuthState::GracePeriod:
            status = "Looking for an activated unIO on your LAN…";
            break;
        case orchestrator::AuthState::SignedOut:
            status = "Sign in (Access tab) on this or any other PC "
                     "to activate the mesh.";
            break;
        case orchestrator::AuthState::SignedIn:
            status = "Searching for peers on your LAN…";
            break;
    }

    ImGui::PushFont(theme::font::body_sm);
    const float w = ImGui::CalcTextSize(status).x;
    center_cursor_x(w);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::palette::paper_muted);
    ImGui::TextUnformatted(status);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/// @brief Alone-state: welcome hero with vertical bias.
void render_alone_state(orchestrator::IOrchestrator& orch) {
    constexpr float kBlockHeight = 210.0f;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float top = std::max(theme::space::xl,
                               (avail_h - kBlockHeight) * 0.35f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + top);

    render_title();
    ImGui::Spacing();
    ImGui::Spacing();
    render_subtitle();
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();
    render_status(orch);
}


/// @brief Render a card with rounded corners and the requested
/// background colour. Calls @p body inside the card.
///
/// Reserves the card's full footprint via a single Dummy() up
/// front so the parent window's content rect tracks the bottom
/// edge correctly (otherwise ImGui prints the "code uses
/// SetCursorPos to extend window/parent boundaries" warning when
/// the body's BeginChild only registers the *inner* rect, not
/// the rounded chrome around it).
template <typename Body>
void render_widget_card(const char* id, float width, float height, Body body) {
    constexpr float kPadX = 18.0f;
    constexpr float kPadY = 16.0f;

    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // 1. Reserve the full card footprint in the parent layout.
    ImGui::Dummy(ImVec2(width, height));

    // 2. Paint the card surface + outline at the reserved origin.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 bot(origin.x + width, origin.y + height);
    dl->AddRectFilled(
        origin, bot,
        ImGui::ColorConvertFloat4ToU32(theme::palette::paper_widget_bg),
        theme::radius::lg);
    dl->AddRect(
        origin, bot,
        ImGui::ColorConvertFloat4ToU32(theme::palette::paper_widget_border),
        theme::radius::lg, ImDrawFlags_None, 1.0f);

    // 3. Body inside a transparent child positioned over the card.
    // ItemSpacing is restored to a sensible default — running_state
    // pushes (0, 0) around the card stack so card heights sum
    // exactly to avail_h, but that 0 spacing would suffocate the
    // body's own widgets (forms, lists, etc.).
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + kPadX, origin.y + kPadY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    // NoScrollWithMouse on the body — wheel events inside the card
    // belong to the inner list child (if any); the body itself
    // must never accept scroll, so the header + button can't drift
    // when content slightly overflows.
    ImGui::BeginChild(
        "##widget-body",
        ImVec2(width - 2.0f * kPadX, height - 2.0f * kPadY),
        ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_NoScrollWithMouse);
    body();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopID();

    // 4. Restore cursor to the card's bottom-left (within the
    // already-reserved rect, so no boundary warning) for the
    // caller's next layout step.
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
}

/// @brief One workspace row inside the workspaces widget — drawn
/// as a sub-card with rounded corners (paper_surface fill). The
/// card hosts a title row (name + Edit pill on the right) and an
/// indented member list below.
void render_workspace_row(
    const orchestrator::Workspace& ws,
    const std::unordered_map<std::string, orchestrator::Peer>& peer_index,
    bool& edit_clicked,
    std::string& edit_id) {
    constexpr float kSubPadX = 14.0f;
    constexpr float kSubPadY = 10.0f;
    constexpr float kDot     = 9.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Channel split so we can draw the card background *under* the
    // content after laying it out (otherwise we'd need to
    // pre-compute the row's exact pixel height up front).
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  card_w = ImGui::GetContentRegionAvail().x;

    // Top padding inside the card.
    ImGui::Dummy(ImVec2(card_w, kSubPadY));
    ImGui::Indent(kSubPadX);

    // Title line.
    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text,
                       "%s",
                       ws.name.empty() ? "Workspace" : ws.name.c_str());
    ImGui::PopFont();

    // Edit pill at the card's right edge (account for inner pad).
    const float btn_w = ImGui::CalcTextSize("Edit").x + 28.0f;
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(
        origin.x + card_w - kSubPadX - btn_w,
        ImGui::GetCursorScreenPos().y));
    if (pill_button((std::string("Edit##ws-row-") + ws.id).c_str(),
                     PillVariant::Secondary)) {
        edit_clicked = true;
        edit_id      = ws.id;
    }

    // Members — indented one notch deeper than the card padding.
    ImGui::Indent(theme::space::sm);
    if (ws.members.empty()) {
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_faint,
                           "No computers yet. Click Edit to add some.");
        ImGui::PopFont();
    } else {
        std::vector<std::string> sorted(ws.members.begin(), ws.members.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& mid : sorted) {
            std::string display_name = mid;
            bool        online       = false;
            if (auto it = peer_index.find(mid); it != peer_index.end()) {
                display_name = it->second.display_name.empty()
                               ? it->second.machine_id
                               : it->second.display_name;
                online       = it->second.online;
            }
            const ImVec2 c0 = ImGui::GetCursorScreenPos();
            ImVec4 col = machine_color(mid);
            if (!online) col.w = 0.4f;
            dl->AddCircleFilled(
                ImVec2(c0.x + kDot * 0.5f,
                       c0.y + ImGui::GetTextLineHeight() * 0.5f),
                kDot * 0.5f,
                ImGui::ColorConvertFloat4ToU32(col),
                20);
            ImGui::Dummy(ImVec2(kDot + theme::space::sm,
                                ImGui::GetTextLineHeight()));
            ImGui::SameLine();
            ImGui::PushFont(theme::font::body_sm);
            ImGui::TextColored(theme::palette::paper_text,
                               "%s", display_name.c_str());
            ImGui::PopFont();
        }
    }
    ImGui::Unindent(theme::space::sm);
    ImGui::Unindent(kSubPadX);

    // Bottom padding inside the card.
    ImGui::Dummy(ImVec2(card_w, kSubPadY));

    // Now we know the card's bottom — draw the rounded background
    // on channel 0, behind the content rendered above.
    const ImVec2 endp = ImGui::GetCursorScreenPos();
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(
        origin,
        ImVec2(origin.x + card_w, endp.y),
        ImGui::ColorConvertFloat4ToU32(theme::palette::paper_surface),
        theme::radius::md);
    dl->ChannelsMerge();

    // Inter-card spacer.
    ImGui::Dummy(ImVec2(0.0f, theme::space::sm));
}

/// @brief Render one centred line of body_sm text inside the
/// caller's content region.
void center_text_line(const char* text, ImVec4 color) {
    const float region_w = ImGui::GetContentRegionAvail().x;
    const float text_w   = ImGui::CalcTextSize(text).x;
    if (region_w > text_w) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                             + (region_w - text_w) * 0.5f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

/// @brief Back-arrow icon button — used as a "Back" affordance
/// when the workspaces card is in inline-manager mode. Renders a
/// proper left arrow (shaft + head) so it reads as an arrow
/// rather than a bare chevron. No background fill; ink turns
/// lilac on hover.
bool back_arrow_button(const char* id, float size) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##back", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);
    const ImVec4 col = hovered ? theme::palette::lilac
                               : theme::palette::paper_text;
    const ImU32  ink = ImGui::ColorConvertFloat4ToU32(col);
    constexpr float kStroke = 2.0f;

    const float shaft_half = size * 0.32f;
    const float head_dx    = size * 0.18f;
    const float head_dy    = size * 0.18f;
    const ImVec2 tip(c.x - shaft_half, c.y);
    // Horizontal shaft.
    dl->AddLine(tip, ImVec2(c.x + shaft_half, c.y), ink, kStroke);
    // Two head edges forming the arrowhead at the tip.
    dl->AddLine(tip, ImVec2(tip.x + head_dx, tip.y - head_dy), ink, kStroke);
    dl->AddLine(tip, ImVec2(tip.x + head_dx, tip.y + head_dy), ink, kStroke);
    ImGui::PopID();
    return clicked;
}

/// @brief Circular "+" button — used in the workspace widget's
/// empty state and top-right corner to start a new workspace.
/// Returns @c true on click. The fill + ink colours track the
/// current global Alpha so wrapping the call in
/// ImGui::BeginDisabled() automatically dims the button.
bool plus_button(const char* id, float size) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##plus", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);

    const float alpha = ImGui::GetStyle().Alpha;
    ImVec4 fill = hovered ? theme::palette::lilac_hover
                          : theme::palette::lilac;
    fill.w *= alpha;
    dl->AddCircleFilled(c, size * 0.5f,
                        ImGui::ColorConvertFloat4ToU32(fill), 32);

    const float arm = size * 0.28f;
    ImVec4 ink = theme::palette::paper_bg;
    ink.w *= alpha;
    const ImU32 ink_col = ImGui::ColorConvertFloat4ToU32(ink);
    dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), ink_col, 2.5f);
    dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), ink_col, 2.5f);
    ImGui::PopID();
    return clicked;
}

/// @brief "Workspaces" widget body — count + first N workspaces +
/// "Show all" pinned right.
void render_workspaces_widget_body(
    const std::vector<orchestrator::Workspace>& items,
    const std::unordered_map<std::string, orchestrator::Peer>& peer_index,
    bool overflow,
    bool can_create,
    bool& open_all_ws,
    bool& edit_clicked,
    std::string& edit_id,
    bool& create_clicked) {

    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text,
                       "Workspaces · %zu", items.size());
    ImGui::PopFont();

    // Right-aligned button cluster on the header line. With existing
    // workspaces, the "+" lives here permanently — disabled (but
    // still rendered) when there aren't enough free computers, with
    // a hover tooltip explaining why. The optional "Show all" pill
    // sits to the left of "+".
    if (!items.empty()) {
        // Title font is body_lg — measure it for vertical centring
        // of the right-side controls. Save before PopFont (ImGui
        // doesn't expose the previous font's metrics afterward).
        ImGui::PushFont(theme::font::body_lg);
        const float title_h = ImGui::GetTextLineHeight();
        ImGui::PopFont();

        // Plus button sized to the title line so the two visually
        // share the same vertical centre. Show-all pill uses the
        // standard pill height which is already close to body_lg.
        const float kPlusSize    = title_h + 4.0f;
        const float show_all_w   =
            overflow ? ImGui::CalcTextSize("Show all").x + 36.0f
                     : 0.0f;
        const float gap_w        = overflow ? theme::space::sm : 0.0f;
        const float right_block_w = kPlusSize + gap_w + show_all_w;

        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                             + ImGui::GetContentRegionAvail().x - right_block_w);

        if (overflow) {
            if (pill_button("Show all##ws", PillVariant::Secondary)) {
                open_all_ws = true;
            }
            ImGui::SameLine(0.0f, theme::space::sm);
        }

        if (!can_create) ImGui::BeginDisabled();
        if (plus_button("ws-create-top", kPlusSize)) {
            create_clicked = true;
        }
        const bool plus_hovered =
            ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        if (!can_create) ImGui::EndDisabled();

        if (!can_create && plus_hovered) {
            // Borderless tooltip — the default frame border on the
            // tooltip window read as a hard outline that clashed
            // with the soft surrounding card chrome.
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::SetTooltip("No more available computers");
            ImGui::PopStyleVar();
        }
    }

    ImGui::Spacing();

    if (items.empty()) {
        constexpr float kPlusSize = 36.0f;
        constexpr float kGap      = 12.0f;

        // Empty-state block: hint line + "+" button (or a different
        // hint when there aren't enough free computers).
        ImGui::PushFont(theme::font::body_sm);
        const float text_h = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        const float block_h = text_h + (can_create ? kGap + kPlusSize : 0.0f);
        const float remaining_h = ImGui::GetContentRegionAvail().y;
        if (remaining_h > block_h) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY()
                                 + (remaining_h - block_h) * 0.5f);
        }

        ImGui::PushFont(theme::font::body_sm);
        center_text_line(
            can_create
                ? "No workspace yet."
                : "Need at least 2 available computers to create a workspace.",
            theme::palette::paper_faint);
        ImGui::PopFont();

        if (can_create) {
            ImGui::Dummy(ImVec2(0.0f, kGap));
            const float region_w = ImGui::GetContentRegionAvail().x;
            if (region_w > kPlusSize) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                                     + (region_w - kPlusSize) * 0.5f);
            }
            if (plus_button("ws-create", kPlusSize)) {
                create_clicked = true;
            }
        }
        return;
    }

    for (const auto& ws : items) {
        render_workspace_row(ws, peer_index, edit_clicked, edit_id);
    }
}

/// @brief Render one peer row inside a rounded sub-card (matches
/// the workspace-row card style). Card hosts a coloured dot,
/// the display name, and the workspace tag ("Available" when
/// not in any workspace). Online state attenuates the dot alpha
/// so paired-but-offline reads as faded.
void render_peer_row(const orchestrator::Peer& p,
                     const std::string& workspace_label) {
    constexpr float kSubPadX = 14.0f;
    constexpr float kSubPadY = 3.0f;
    constexpr float kDot     = 10.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float  card_w = ImGui::GetContentRegionAvail().x;

    // ItemSpacing 0 *inside* the card so the bg rect (origin → endp)
    // hugs the content tightly; the parent's ItemSpacing (pushed
    // around the loop) handles the gap *between* cards.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::Dummy(ImVec2(card_w, kSubPadY));
    ImGui::Indent(kSubPadX);

    // Coloured dot.
    const ImVec2 c0 = ImGui::GetCursorScreenPos();
    ImVec4 col = machine_color(p.machine_id);
    if (!p.online) col.w = 0.4f;
    dl->AddCircleFilled(
        ImVec2(c0.x + kDot * 0.5f,
               c0.y + ImGui::GetTextLineHeight() * 0.5f),
        kDot * 0.5f,
        ImGui::ColorConvertFloat4ToU32(col),
        20);
    ImGui::Dummy(ImVec2(kDot + theme::space::sm,
                        ImGui::GetTextLineHeight()));

    // Name.
    ImGui::SameLine();
    ImGui::TextColored(theme::palette::paper_text,
                       "%s", p.display_name.c_str());

    // Workspace tag inline after the name.
    ImGui::SameLine();
    ImGui::PushFont(theme::font::body_sm);
    ImGui::TextColored(theme::palette::paper_faint, "·");
    ImGui::SameLine();
    ImGui::TextColored(workspace_label.empty() ? theme::palette::paper_faint
                                               : theme::palette::lilac,
                       "%s",
                       workspace_label.empty() ? "Available"
                                               : workspace_label.c_str());
    ImGui::PopFont();

    ImGui::Unindent(kSubPadX);
    ImGui::Dummy(ImVec2(card_w, kSubPadY));

    // Background on channel 0 — bg rect extends from origin to a
    // *computed* bottom (not GetCursorScreenPos, which would
    // include the upcoming gap Dummy). Smaller corner radius keeps
    // the rounding-to-height ratio balanced for these compact
    // chips.
    const float card_h = 2.0f * kSubPadY + ImGui::GetTextLineHeight();
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(
        origin,
        ImVec2(origin.x + card_w, origin.y + card_h),
        ImGui::ColorConvertFloat4ToU32(theme::palette::paper_surface),
        theme::radius::sm);
    dl->ChannelsMerge();

    // Inter-card gap — explicit Dummy with the same 0-spacing so
    // its height becomes the literal gap (no doubling). The
    // surrounding paper_widget_bg shows through here, visually
    // separating consecutive chips.
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    ImGui::PopStyleVar();
}

/// @brief "Connected computers" widget body — header + Expand /
/// Collapse pill on the right + a scrollable list of peer rows.
/// The list reserves space for the scrollbar so its rows don't
/// shift width when content overflows; the scrollbar grab fades
/// in when the list is hovered.
void render_connected_widget_body(
    const std::vector<orchestrator::Peer>& peers,
    const std::unordered_map<std::string, std::string>& peer_to_workspace,
    bool expanded,
    float row_h,
    std::size_t preview_rows,
    bool& toggle_clicked) {

    ImGui::PushFont(theme::font::body_lg);
    ImGui::TextColored(theme::palette::paper_text,
                       "Connected computers · %zu", peers.size());
    ImGui::PopFont();

    // Expand pill is meaningful only when the list overflows the
    // preview area — otherwise the user can already see every
    // peer. While expanded, show Collapse regardless so the user
    // can return to the preview height.
    const bool overflow = peers.size() > preview_rows;
    const bool show_toggle = expanded || overflow;
    if (show_toggle) {
        ImGui::SameLine();
        const char* btn_label = expanded ? "Collapse" : "Expand";
        const float btn_w = ImGui::CalcTextSize(btn_label).x + 36.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                             + ImGui::GetContentRegionAvail().x - btn_w);
        if (pill_button((std::string(btn_label) + "##connected-toggle").c_str(),
                         PillVariant::Secondary)) {
            toggle_clicked = true;
        }
    }

    ImGui::Spacing();

    if (peers.empty()) {
        ImGui::PushFont(theme::font::body_sm);
        ImGui::TextColored(theme::palette::paper_faint,
                           "Waiting for computers to connect…");
        ImGui::PopFont();
        return;
    }

    // Hover state from the previous frame — drives scrollbar grab
    // alpha so the bar fades in only when the cursor is over the
    // list, but the reserved width stays constant.
    static bool list_hovered = false;
    const float scroll_a = list_hovered ? 0.55f : 0.0f;
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,
                          ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,
                          ImVec4(0.62f, 0.62f, 0.68f, scroll_a));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,
                          ImVec4(0.50f, 0.50f, 0.56f, scroll_a));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,
                          ImVec4(0.40f, 0.40f, 0.46f, scroll_a));

    const float list_h = expanded
        ? ImGui::GetContentRegionAvail().y
        : static_cast<float>(preview_rows) * row_h + 4.0f;
    // No AlwaysVerticalScrollbar — the scrollbar shows only when
    // content actually overflows. The hover-fade colours above
    // make the bar subtle when it does appear.
    ImGui::BeginChild("##connected-list",
                      ImVec2(0.0f, list_h),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);
    list_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

    // The gap between consecutive chips is added inside
    // render_peer_row itself (see explicit Dummy(0, 6) at the end
    // there) so we don't need a per-loop ItemSpacing push here.
    for (const auto& p : peers) {
        std::string label;
        if (auto it = peer_to_workspace.find(p.machine_id);
            it != peer_to_workspace.end()) {
            label = it->second;
        }
        render_peer_row(p, label);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(4);
}

/// @brief Running state: two stacked widgets — Workspaces (top,
/// flexible height) and Connected computers (bottom, fixed
/// preview height of N rows). Clicking "Expand" on the bottom
/// widget grows it to cover both areas (the workspaces widget
/// hides for the duration). Workspace creation is gated on having
/// at least 2 unassigned computers.
void render_running_state(orchestrator::IOrchestrator& orch) {
    constexpr std::size_t kWorkspacePreviewMax = 4;
    constexpr std::size_t kConnectedPreviewMax = 5;
    constexpr std::size_t kMinFreePcs          = 2;
    // Per-row footprint: card (text line ~14 + 2× pad-Y of 3 = 20)
    // + 6 px inter-card gap. 28 leaves a touch of slack so 5 rows
    // fit cleanly without the bottom card clipping.
    constexpr float       kRowHeight           = 28.0f;
    constexpr float       kCardChromeH         = 16.0f * 2.0f      // card pad-y
                                               + 28.0f             // header line height-ish
                                               + 4.0f;             // post-header spacing
    // Gap between the two cards == top + bottom page margin
    // (theme::space::page_y), so all three vertical breathing
    // gaps inside the tab are equal.
    const float           kCardGap             = theme::space::page_y;

    static workspaces::ViewState workspaces_view;
    static bool                  connected_expanded = false;
    static bool                  ws_manager_open    = false;
    static bool                  ws_scroll_top      = false;

    const auto all_peers       = orch.peers();
    const auto workspaces_list = orch.workspaces();
    const auto assignments     = orch.pc_workspace_assignments();

    // {machine_id → workspace name} — used by both widgets.
    std::unordered_map<std::string, std::string> peer_to_workspace;
    {
        std::unordered_map<std::string, std::string> id_to_name;
        for (const auto& ws : workspaces_list) id_to_name[ws.id] = ws.name;
        for (const auto& [mid, ws_id] : assignments) {
            if (auto it = id_to_name.find(ws_id); it != id_to_name.end()) {
                peer_to_workspace[mid] = it->second;
            }
        }
    }
    const std::size_t unassigned_count =
        all_peers.size() - peer_to_workspace.size();
    const bool can_create = unassigned_count >= kMinFreePcs;

    const bool ws_overflow = workspaces_list.size() > kWorkspacePreviewMax;
    std::vector<orchestrator::Workspace> ws_preview;
    ws_preview.reserve(std::min(workspaces_list.size(), kWorkspacePreviewMax));
    for (std::size_t i = 0;
         i < workspaces_list.size() && i < kWorkspacePreviewMax; ++i) {
        ws_preview.push_back(workspaces_list[i]);
    }

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float card_w  = avail_w;

    bool        open_all_ws       = false;
    bool        edit_clicked      = false;
    bool        create_clicked    = false;
    bool        toggle_clicked    = false;
    bool        back_clicked      = false;
    std::string edit_id;

    // ItemSpacing 0 for the card section so the gap between the
    // two stacked cards is exactly kCardGap (the default ~4 px
    // ItemSpacing.y between siblings would otherwise make the
    // sum overshoot avail_h and the bottom card would clip).
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    if (connected_expanded) {
        // Connected widget fills the entire page area.
        const float card_h = std::max(kCardChromeH + kRowHeight, avail_h);
        render_widget_card("connected", card_w, card_h, [&] {
            render_connected_widget_body(
                all_peers, peer_to_workspace,
                /*expanded=*/true,
                kRowHeight, kConnectedPreviewMax,
                toggle_clicked);
        });
    } else {
        // Connected widget at fixed preview height; workspaces
        // takes the remainder above it. Extra ~24 px of slack so
        // the rounded card bottom sits clearly below the last list
        // row instead of clipping into it.
        const float connected_h =
            kCardChromeH + kConnectedPreviewMax * kRowHeight + 24.0f;
        const float workspaces_h = std::max(
            kCardChromeH + kRowHeight,
            avail_h - kCardGap - connected_h);

        render_widget_card("workspaces", card_w, workspaces_h, [&] {
            if (ws_manager_open) {
                // Inline manager. Header row (back arrow + title)
                // stays pinned; only the body below scrolls so the
                // user always knows what view they're in even with
                // a tall form.
                const bool in_form =
                    (workspaces_view.mode == workspaces::Mode::Create
                     || workspaces_view.mode == workspaces::Mode::Edit);

                const float kArrow = 28.0f;
                if (back_arrow_button("ws-back", kArrow)) {
                    back_clicked = true;
                }
                if (in_form) {
                    ImGui::SameLine(0.0f, theme::space::sm);
                    ImGui::PushFont(theme::font::body_lg);
                    const float text_h = ImGui::GetTextLineHeight();
                    // Vertically centre the title against the
                    // 28 px arrow button.
                    const float dy = (kArrow - text_h) * 0.5f;
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + dy);
                    ImGui::TextColored(
                        theme::palette::paper_text,
                        workspaces_view.mode == workspaces::Mode::Edit
                            ? "Edit workspace"
                            : "New workspace");
                    ImGui::PopFont();
                }
                hairline();
                ImGui::Dummy(ImVec2(0.0f, theme::space::xs));

                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
                ImGui::BeginChild(
                    "##ws-manager-scroll",
                    ImGui::GetContentRegionAvail(),
                    ImGuiChildFlags_None,
                    ImGuiWindowFlags_None);
                // Scroll to top whenever the user just transitioned
                // into Create/Edit — start fresh at the form header
                // even if a previous form was scrolled down.
                if (ws_scroll_top) {
                    ImGui::SetScrollY(0.0f);
                    ws_scroll_top = false;
                }
                // workspaces::render returns true after Delete — the
                // workspace the user was editing no longer exists,
                // so route them back to the main preview.
                if (workspaces::render(orch, workspaces_view)) {
                    back_clicked = true;
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
            } else {
                // Build {machine_id → Peer} once per frame so the
                // workspace rows can resolve member ids → display
                // names + online state.
                std::unordered_map<std::string, orchestrator::Peer> peer_index;
                peer_index.reserve(all_peers.size());
                for (const auto& p : all_peers) {
                    peer_index.emplace(p.machine_id, p);
                }
                render_workspaces_widget_body(
                    ws_preview, peer_index, ws_overflow, can_create,
                    open_all_ws, edit_clicked, edit_id, create_clicked);
            }
        });
        ImGui::Dummy(ImVec2(card_w, kCardGap));
        render_widget_card("connected", card_w, connected_h, [&] {
            render_connected_widget_body(
                all_peers, peer_to_workspace,
                /*expanded=*/false,
                kRowHeight, kConnectedPreviewMax,
                toggle_clicked);
        });
    }

    ImGui::PopStyleVar();

    if (toggle_clicked) connected_expanded = !connected_expanded;

    // Open the inline manager on any click that needs it; the
    // form's own Save/Cancel/Delete reset workspaces_view.mode
    // back to List but keep the manager open so the user can do
    // more — they leave by clicking Back.
    if (open_all_ws || edit_clicked || create_clicked) {
        ws_manager_open = true;
    }
    if (edit_clicked) {
        if (auto ws = orch.workspace(edit_id); ws) {
            workspaces::start_edit(*ws, workspaces_view);
            ws_scroll_top = true;
        }
    }
    if (create_clicked) {
        workspaces::start_create(workspaces_view);
        ws_scroll_top = true;
    }
    if (back_clicked) {
        workspaces_view.reset();
        ws_manager_open = false;
    }
}

}  // namespace

void render(orchestrator::IOrchestrator& orch) {
    bool any_remote = false;
    for (const auto& p : orch.peers()) {
        if (!p.is_local) { any_remote = true; break; }
    }

    if (any_remote && orch.auth_state() == orchestrator::AuthState::SignedIn) {
        render_running_state(orch);
    } else {
        render_alone_state(orch);
    }
}

}  // namespace unio_ui::ui::activity
