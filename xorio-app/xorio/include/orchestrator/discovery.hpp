/// @file discovery.hpp
/// @brief LAN peer discovery (mDNS + manual-invite fallback).
#pragma once

#include "orchestrator/crypto/crypto.hpp"
#include "orchestrator/display.hpp"
#include "orchestrator/workspace.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xorio::orchestrator {

/// @brief Signed payload broadcast on mDNS.
///
/// Carries the minimum metadata a listener needs to decide
/// whether to attempt pairing: public-key fingerprint, a display
/// name, and candidate transport endpoints. All bytes below the
/// signature are covered by it.
struct DiscoveryAnnouncement {
    crypto::PairingPublicKey public_key;
    std::string              machine_id;   ///< Hex of @c public_key.
    std::string              display_name;
    std::string              address;      ///< LAN IP literal.
    std::uint16_t            control_port = 0;
    /// @brief File-transfer data-channel port. 0 when the
    /// announcer omitted it (legacy peer); listeners then
    /// fall back to @c control_port for file frames at the
    /// cost of cursor latency during transfers.
    std::uint16_t            data_port    = 0;
    std::uint64_t            version_ns   = 0;
    /// @brief Announcing peer reports a signed-in (authorized)
    /// user. Listeners use this to auto-activate themselves when
    /// any mesh peer is already authorized — see the orchestrator
    /// façade's auto-authorize path.
    bool                     authed = false;

    /// @brief Real display geometry from the announcer's local
    /// probe. Empty when the wire payload omits the field; the
    /// orchestrator falls back to a synthesised placeholder in
    /// that case.
    std::vector<Display>     displays;

    /// @brief Announcer's full workspace catalogue (including
    /// tombstones). The orchestrator hands this to
    /// @ref IWorkspaceManager::merge_remote so peers converge on
    /// the same view via last-writer-wins. Empty when the wire
    /// payload omits the workspaces extension.
    std::vector<Workspace>   workspaces;

    crypto::Signature        signature;
};

/// @brief Multi-homed LAN discovery service.
///
/// Listens + announces on every LAN-facing network interface; the
/// orchestrator receives a notification whenever a peer appears,
/// updates its claim, or times out.
class IDiscovery {
public:
    /// @brief Callback signature for peer-lifecycle events.
    using PeerObservedFn = std::function<void(const DiscoveryAnnouncement&)>;
    using PeerLostFn     = std::function<void(const std::string& machine_id)>;

    virtual ~IDiscovery() = default;

    /// @brief Begin announcing + listening.
    /// @param on_peer_observed  Fires on each announcement received
    ///                          (including refreshes from known peers).
    /// @param on_peer_lost      Fires when a peer's announcements
    ///                          age past the staleness threshold.
    virtual void start(PeerObservedFn on_peer_observed,
                       PeerLostFn     on_peer_lost) = 0;

    /// @brief Halt announcements; close sockets.
    virtual void stop() = 0;

    /// @brief Send an announce datagram right now, outside the
    /// regular 2-second tick. Used when the orchestrator wants a
    /// state change (Identify-counter bump, sign-in, …) to reach
    /// peers without the normal 0–2 s wait.
    virtual void trigger_announce_now() = 0;

    /// @brief Accept a manual invitation code in place of mDNS.
    /// Used when the LAN blocks multicast.
    virtual void accept_manual_invite(const std::string& invite_code) = 0;
};

}  // namespace xorio::orchestrator
