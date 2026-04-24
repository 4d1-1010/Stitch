/// @file orchestrator.hpp
/// @brief Public façade the UI consumes. Queries + actions only;
/// push notifications arrive through @ref OrchestratorCallbacks.
#pragma once

#include "orchestrator/auth_state.hpp"
#include "orchestrator/callbacks.hpp"
#include "orchestrator/display.hpp"
#include "orchestrator/peer.hpp"
#include "orchestrator/stream.hpp"

#include <memory>
#include <string>
#include <vector>

namespace unio_ui::orchestrator {

/// @brief Single entry point the UI depends on.
///
/// All query methods are safe to call every frame; the façade
/// returns snapshots of internal state guarded by a mutex.
/// Action methods return immediately and complete asynchronously
/// — results surface through the callbacks registered at
/// construction.
class IOrchestrator {
public:
    virtual ~IOrchestrator() = default;

    // ── Identity queries ───────────────────────────────────
    virtual std::string local_machine_id() const = 0;
    virtual std::string local_display_name() const = 0;
    virtual AuthState   auth_state() const = 0;

    // ── Mesh queries ───────────────────────────────────────
    virtual std::vector<Peer>    peers() const = 0;
    virtual std::vector<Display> displays() const = 0;
    virtual StreamState stream_state(StreamId id) const = 0;

    // ── Actions ────────────────────────────────────────────

    /// @brief Start a stream from @p src to @p dst.
    /// @return A new @ref StreamId on success; @c StreamId{0} if
    /// the request failed synchronous validation. Subsequent
    /// progress surfaces through @c on_stream_started /
    /// @c on_stream_failed callbacks.
    virtual StreamId start_stream(DisplayRef src, DisplayRef dst,
                                  RoutingMode mode) = 0;

    /// @brief Stop the stream identified by @p id.
    virtual void stop_stream(StreamId id) = 0;

    /// @brief Initiate an outgoing pairing request.
    /// @param machine_id  Remote peer fingerprint.
    /// @param invite_code Shared secret shown on the remote
    ///                    peer's Access screen (QR / 6-digit PIN).
    virtual void request_pair(const std::string& machine_id,
                              const std::string& invite_code) = 0;

    /// @brief Accept an incoming pairing request.
    virtual void accept_pairing(const std::string& machine_id) = 0;

    /// @brief Reject an incoming pairing request.
    virtual void reject_pairing(const std::string& machine_id,
                                const std::string& reason) = 0;

    /// @brief Revoke an existing pairing. Closes the control
    /// connection and drops the peer's CRDT slot.
    virtual void unpair(const std::string& machine_id) = 0;
};

/// @brief Construct a mock orchestrator populated with simulated
/// mesh state (Diana appears after a short delay, local peer
/// transitions from GracePeriod to SignedIn).
///
/// @param callbacks  Push callbacks invoked from the worker
/// thread as simulated events fire. Any member may be @c nullptr.
///
/// @return Owned pointer; holds its worker thread until destroyed.
std::unique_ptr<IOrchestrator>
make_mock(const OrchestratorCallbacks& callbacks);

}  // namespace unio_ui::orchestrator
