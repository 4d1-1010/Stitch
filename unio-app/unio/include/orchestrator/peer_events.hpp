/// @file peer_events.hpp
/// @brief Peer-lifecycle handler — translates LAN-discovery events
/// into mesh + UI side-effects.
///
/// Owned by @ref FacadeOrchestrator (`src/orchestrator/orchestrator.cpp`).
/// Borrows references to the façade's mesh + pairing + control
/// sub-modules, the callback set the UI registered, and the peer
/// map the façade publishes. Holds no state of its own beyond
/// those references — lifetime is strictly tied to the façade.
#pragma once

#include "orchestrator/callbacks.hpp"
#include "orchestrator/control_connection.hpp"
#include "orchestrator/discovery.hpp"
#include "orchestrator/mesh_crdt.hpp"
#include "orchestrator/pairing_manager.hpp"
#include "orchestrator/peer.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace unio_ui::orchestrator {

/// @brief Handles the peer-observed / peer-lost lifecycle.
///
/// Single responsibility: react to discovery datagrams. The
/// façade installs lambda shims around @ref handle_peer_observed
/// and @ref handle_peer_lost when starting LAN discovery. All
/// state mutation happens through the borrowed references; the
/// handler itself is reentrant-safe via the shared peers mutex.
class PeerEventHandler {
public:
    PeerEventHandler(IMeshCrdt&                              mesh,
                     IPairingManager&                        pairing,
                     IControlConnectionManager&              control,
                     OrchestratorCallbacks&                  callbacks,
                     std::atomic<bool>&                      access_authorized,
                     std::mutex&                             peers_mtx,
                     std::unordered_map<std::string, Peer>&  peers)
        : mesh_(mesh),
          pairing_(pairing),
          control_(control),
          callbacks_(callbacks),
          access_authorized_(access_authorized),
          peers_mtx_(peers_mtx),
          peers_(peers) {}

    /// @brief Process a discovery announcement.
    /// First-sighting fires `on_peer_joined`; refreshes update the
    /// cached row + republish caps. Auto-activates the local access
    /// gate when any mesh peer reports `authed:true`.
    void handle_peer_observed(const DiscoveryAnnouncement& a);

    /// @brief Process a peer-lost (TTL elapsed) event — drop the
    /// peer + notify the UI.
    void handle_peer_lost(const std::string& machine_id);

private:
    /// @brief Fallback caps record for peers whose announce omitted
    /// the displays extension (legacy Python instances or pre-R2
    /// builds). Real-display peers supply their own geometry on
    /// the wire.
    static CapsRecord synthesize_peer_caps(const std::string& machine_id,
                                           const std::string& display_name);

    IMeshCrdt&                              mesh_;
    IPairingManager&                        pairing_;
    IControlConnectionManager&              control_;
    OrchestratorCallbacks&                  callbacks_;
    std::atomic<bool>&                      access_authorized_;
    std::mutex&                             peers_mtx_;
    std::unordered_map<std::string, Peer>&  peers_;
};

}  // namespace unio_ui::orchestrator
