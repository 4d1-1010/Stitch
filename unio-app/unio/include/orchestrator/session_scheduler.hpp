/// @file session_scheduler.hpp
/// @brief Start / stop / failover for active streams.
#pragma once

#include "orchestrator/stream.hpp"

#include <functional>
#include <string>

namespace unio_ui::orchestrator {

/// @brief Outcome of a @ref ISessionScheduler::start call.
enum class StartOutcome {
    Accepted,            ///< Negotiation in progress; see callbacks.
    RefusedNoRoute,      ///< Path selector returned @c RoutingRefusal.
    RefusedUnpaired,     ///< Destination peer is not paired.
    RefusedPeerOffline,  ///< Control connection not established.
    RefusedBusy,         ///< Destination already hosting a conflicting stream.
};

/// @brief Stream-lifecycle scheduler.
///
/// Coordinates path selection, control-plane RPC, media-connection
/// handoff, and failover retries. Owns a @c StreamId namespace;
/// every active stream maps to exactly one scheduler slot.
class ISessionScheduler {
public:
    using StateChangedFn = std::function<void(StreamId, StreamState)>;
    using FailureFn      = std::function<void(StreamId,
                                              const std::string& reason)>;

    virtual ~ISessionScheduler() = default;

    /// @brief Install lifecycle callbacks. May be called once.
    virtual void set_callbacks(StateChangedFn on_state,
                               FailureFn on_failure) = 0;

    /// @brief Begin a new stream.
    /// @return The assigned @ref StreamId on @c Accepted, else
    /// @c StreamId{0} together with the refusal reason.
    virtual std::pair<StreamId, StartOutcome>
        start(DisplayRef src, DisplayRef dst, RoutingMode mode) = 0;

    /// @brief Stop an active stream.
    virtual void stop(StreamId id) = 0;

    /// @brief Current lifecycle state of @p id.
    /// @return @c StreamState::Stopped for unknown ids.
    virtual StreamState state_of(StreamId id) const = 0;
};

}  // namespace unio_ui::orchestrator
