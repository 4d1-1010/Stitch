/*! @file layout.hpp
 *  @brief Two-row layout canvas: PCs on top, displays on bottom.
 *
 *  Port of `unio/apps/layout_panel.py` — the *feature* that
 *  picked Dear ImGui over a native-widget toolkit. The canvas is
 *  custom-rendered via `ImDrawList`.
 *
 *  First cut (this file): rendering only.
 *    - machine_color: CRC32-seeded HSL → RGB, byte-for-byte match
 *      with the Python helper so stale LWW state from a mixed
 *      cluster during the migration shows the same colours.
 *    - Top band: one rectangle per PC, with a colour dot + name.
 *    - Bottom band: one rectangle per display, positioned by its
 *      global_x / global_y × the canvas scale.
 *    - Identity routing lines (PC-node → own displays).
 *
 *  Out of scope here (follow-up commits):
 *    - Edit gestures (drag display, drag line to reroute, swap).
 *    - Non-identity routing (depends on orchestrator stub #79).
 *    - Pan / zoom on the bottom area.
 *    - Identify flash, apply / discard buttons.
 */
#pragma once

namespace unio_ui::orchestrator { class IOrchestrator; }

namespace unio_ui::screens::layout {

/// Render the Layout tab body into the current ImGui content
/// region. Called from `Shell::render_content` when the tab is
/// active. Queries @p orch for peers + displays.
void render(orchestrator::IOrchestrator& orch);

}  // namespace unio_ui::screens::layout
