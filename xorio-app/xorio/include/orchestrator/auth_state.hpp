/// @file auth_state.hpp
/// @brief Authorisation state of the local peer in the mesh.
#pragma once

namespace xorio_ui::orchestrator {

/// @brief Tri-state of the local peer's mesh-access standing.
enum class AuthState {
    GracePeriod,  ///< Discovery still probing after launch.
    SignedOut,    ///< No paired peer has activated us.
    SignedIn,     ///< Signed in locally or activated by a remote peer.
};

}  // namespace xorio_ui::orchestrator
