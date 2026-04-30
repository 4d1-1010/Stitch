/// @file callbacks.hpp
/// @brief Push-side surface the UI registers with the orchestrator.
///
/// The UI provides one @ref OrchestratorCallbacks struct at
/// construction. Each member is either a @c std::function or
/// @c nullptr; the orchestrator invokes the ones that are set.
/// No event queue, no pub-sub — direct callbacks only.
///
/// All callbacks run on the orchestrator's worker thread. The UI
/// is expected to marshal to the render thread if required.
#pragma once

#include "orchestrator/peer.hpp"
#include "orchestrator/stream.hpp"

#include <functional>
#include <string>

namespace xorio::orchestrator {

/// @brief Collected callback pointers the orchestrator may invoke.
struct OrchestratorCallbacks {
    /// @brief A peer entered the mesh (freshly paired or came back online).
    std::function<void(const Peer&)> on_peer_joined;

    /// @brief A peer is no longer reachable (heartbeat missed beyond threshold).
    std::function<void(const std::string& machine_id)> on_peer_left;

    /// @brief A peer's published capabilities changed.
    std::function<void(const std::string& machine_id)> on_peer_capabilities_changed;

    /// @brief Local authorisation state transitioned.
    std::function<void()> on_auth_state_changed;

    /// @brief A stream entered @c Starting state.
    std::function<void(StreamId)> on_stream_started;

    /// @brief A stream terminated non-cleanly.
    std::function<void(StreamId, const std::string& reason)> on_stream_failed;

    /// @brief A degraded stream recovered to @c Running.
    std::function<void(StreamId)> on_stream_recovered;

    /// @brief A stream reached @c Stopped or @c Failed.
    std::function<void(StreamId)> on_stream_stopped;

    /// @brief Another peer requested pairing with us.
    std::function<void(const std::string& machine_id,
                       const std::string& one_time_code)> on_pairing_request;

    /// @brief An outgoing pair request was accepted by the remote peer.
    std::function<void(const std::string& machine_id)> on_pairing_accepted;

    /// @brief An outgoing pair request was rejected.
    std::function<void(const std::string& machine_id,
                       const std::string& reason)> on_pairing_rejected;

    /// @brief Identify was requested — either by the local user
    /// or by a remote peer's announce bumping its counter. The UI
    /// fires the per-monitor fullscreen overlay on the local PC.
    /// Same callback either way so both code paths stay in sync.
    std::function<void()> on_identify_request;
};

}  // namespace xorio::orchestrator
