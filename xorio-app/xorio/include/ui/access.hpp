/// @file access.hpp
/// @brief Access tab — pre-launch hardcoded-key gate.
///
/// Renders the Access screen body. While the orchestrator's
/// @c access_authorized() returns @c false, this screen is the
/// only navigable destination; once @c try_authorize() returns
/// @c true, the rest of the rail unlocks.
///
/// The real licence-token verifier (Ed25519 paste-box +
/// signature check + state machine) replaces this body when the
/// crypto branch lands; the public render() signature stays.
#pragma once

namespace xorio_ui::orchestrator { class IOrchestrator; }

namespace xorio_ui::ui::access {

/// @brief Render the Access tab body.
/// @param orch  Orchestrator consulted for the gate state and
///              for submitting the entered key.
void render(orchestrator::IOrchestrator& orch);

}  // namespace xorio_ui::ui::access
