/// @file pairing_manager.hpp
/// @brief Bilateral trust establishment between peers.
#pragma once

#include "orchestrator/crypto/crypto.hpp"

#include <functional>
#include <string>
#include <vector>

namespace xorio_ui::orchestrator {

/// @brief Persisted record of one paired peer.
struct PairedPeer {
    crypto::PairingPublicKey peer_public_key;
    std::string              peer_machine_id;
    std::string              peer_display_name;
    std::uint64_t            paired_at_ns = 0;
};

/// @brief Outcome of a pairing handshake.
enum class PairingOutcome {
    Accepted,
    Rejected,
    TimedOut,
    Conflict,         ///< Invite code already consumed.
    SignatureInvalid,
};

/// @brief Pairing manager interface.
///
/// Provides the three lifecycle operations (request, accept,
/// reject) plus a persisted list of known peers. The handshake
/// itself runs on the control connection; this interface is
/// the policy seat that mediates it.
class IPairingManager {
public:
    /// @brief Fires when a remote peer requests pairing with us.
    using IncomingRequestFn =
        std::function<void(const std::string& machine_id,
                           const std::string& one_time_code)>;

    /// @brief Fires when a pairing handshake we initiated resolves.
    using OutcomeFn =
        std::function<void(const std::string& machine_id,
                           PairingOutcome outcome,
                           const std::string& reason)>;

    virtual ~IPairingManager() = default;

    /// @brief Install incoming-request and outcome callbacks.
    virtual void set_callbacks(IncomingRequestFn on_incoming,
                               OutcomeFn         on_outcome) = 0;

    /// @brief Initiate pairing with @p machine_id.
    /// @param invite_code  Displayed by the remote peer.
    virtual void request(const std::string& machine_id,
                         const std::string& invite_code) = 0;

    /// @brief Accept a pending incoming pairing request.
    virtual void accept(const std::string& machine_id) = 0;

    /// @brief Reject a pending incoming pairing request.
    virtual void reject(const std::string& machine_id,
                        const std::string& reason) = 0;

    /// @brief Revoke an existing pairing; peer drops back to unknown.
    virtual void unpair(const std::string& machine_id) = 0;

    /// @brief Enumerate currently-paired peers.
    virtual std::vector<PairedPeer> list_paired() const = 0;

    /// @brief @c true iff @p machine_id is in the paired set.
    virtual bool is_paired(const std::string& machine_id) const = 0;
};

}  // namespace xorio_ui::orchestrator
