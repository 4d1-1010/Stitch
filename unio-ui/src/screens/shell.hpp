/*! @file shell.hpp
 *  @brief Main window — top bar + left rail + content area.
 *
 *  Port of `unio/apps/shell.py` (the single top-level Tk window).
 *  First-cut scope:
 *    - Viewport-filling layout (we use the native OS title bar,
 *      not an ImGui one)
 *    - Top bar: wordmark + hostname + connection dot
 *    - Left rail: one @ref theme::rail_button per tab
 *    - Content area: placeholder per tab; each tab body ports
 *      into its own `screens/` cpp file in follow-up commits
 *
 *  Out of scope here (explicit TODOs — task list):
 *    - Session lifecycle (Peer + MeshDiscovery) — orchestrator
 *      concern, not UI. Will arrive on the orchestrator layer
 *      and plug in via a stub interface.
 *    - Account / sign-in tab internals (task follow-up)
 */
#pragma once

namespace unio_ui::orchestrator { class IOrchestrator; }

namespace unio_ui::screens {

/*! @brief The single top-level window of the C++ UI.
 *
 *  One instance owned by the platform app; `render()` runs inside
 *  each frame's `ImGui::NewFrame` / `ImGui::Render` bracket.
 */
class Shell {
public:
    /// Mirror of the Tk app's tab list. Public so callers can
    /// programmatically switch tabs (the tab-desc table in
    /// shell.cpp also needs the enumerators).
    enum class Tab {
        Activity,
        Layout,
        Settings,
        Access,
        Help,
    };

    explicit Shell(orchestrator::IOrchestrator& orch);
    ~Shell() = default;

    /// Call once per frame. Paints the full viewport.
    void render();

private:
    orchestrator::IOrchestrator& orch_;
    Tab current_tab_ = Tab::Activity;

    void render_top_bar();
    void render_rail();
    void render_content();

    // Tab bodies (first cut: placeholders).
    void render_activity();
    void render_layout();
    void render_settings();
    void render_access();
    void render_help();
};

}  // namespace unio_ui::screens
