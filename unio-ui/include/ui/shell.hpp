/// @file shell.hpp
/// @brief Top-level application window: rail + content dispatcher.
#pragma once

namespace unio_ui::orchestrator { class IOrchestrator; }

namespace unio_ui::ui {

/// @brief Root UI surface. Owns tab selection and dispatches to
/// per-tab render functions.
class Shell {
public:
    /// @brief Navigation tab identity.
    enum class Tab {
        Activity,
        Layout,
        Settings,
        Access,
        Help,
    };

    /// @param orch  Orchestrator accessed for per-tab state.
    explicit Shell(orchestrator::IOrchestrator& orch);
    ~Shell() = default;

    /// @brief Render one frame inside the viewport.
    /// Must run between `ImGui::NewFrame` and `ImGui::Render`.
    void render();

private:
    orchestrator::IOrchestrator& orch_;
    Tab current_tab_ = Tab::Activity;

    void render_rail();
    void render_content();

    void render_activity();
    void render_layout();
    void render_settings();
    void render_access();
    void render_help();
};

}  // namespace unio_ui::ui
