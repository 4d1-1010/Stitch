/// @file stream.hpp
/// @brief Stream identity + lifecycle types.
#pragma once

#include <cstdint>
#include <string>

namespace xorio::orchestrator {

/// @brief Opaque identifier for an active routing line.
///
/// Assigned by the orchestrator at @c start_stream and carried
/// through every RPC + media-connection setup for that stream.
struct StreamId {
    std::uint64_t value = 0;

    friend bool operator==(StreamId a, StreamId b) { return a.value == b.value; }
    friend bool operator!=(StreamId a, StreamId b) { return a.value != b.value; }
};

/// @brief Reference to a specific display on a specific peer.
struct DisplayRef {
    std::string machine_id;
    std::string monitor_id;
};

/// @brief Routing intent requested by the UI.
enum class RoutingMode {
    Windowed,        ///< One-way windowed mirror on the sink.
    FullscreenOneWay,///< Cover the sink monitor, source keeps its display.
    FullscreenSwap,  ///< Cover the sink AND the source (desktop-swap).
};

/// @brief Observable state of one stream across its lifetime.
enum class StreamState {
    Negotiating,     ///< Control RPC in flight; media connection not yet opened.
    Starting,        ///< Media connection opened; waiting for first key frame.
    Running,         ///< Frames flowing.
    Recovering,      ///< Packet loss / congestion; retrying.
    Stopping,        ///< Teardown requested.
    Stopped,         ///< Terminal; resources released.
    Failed,          ///< Terminal; non-recoverable error.
};

}  // namespace xorio::orchestrator
