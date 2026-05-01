/// @file media_connection.hpp
/// @brief Per-session QUIC connection carrying frame bytes.
#pragma once

#include "orchestrator/stream.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace xorio::orchestrator {

/// @brief Parameters negotiated over the control connection
/// before the media connection is opened.
struct MediaConnectionParams {
    StreamId      stream_id;
    std::string   peer_machine_id;
    std::string   peer_address;
    std::uint16_t peer_port = 0;
    std::string   codec;         ///< e.g. "h264".
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::uint32_t fps    = 0;
};

/// @brief Lifecycle transitions for one media connection.
enum class MediaConnectionState {
    Opening,
    Open,
    Closed,
    Failed,
};

/// @brief Per-session QUIC connection handle.
///
/// Owned by the session scheduler; one instance per active
/// @ref StreamId. Unlike @c unio-pipe's @c StreamManager, this
/// interface does not own the encode/decode pipeline — only the
/// transport. The scheduler wires the two together.
class IMediaConnection {
public:
    virtual ~IMediaConnection() = default;

    virtual StreamId             stream_id() const = 0;
    virtual MediaConnectionState state() const = 0;

    /// @brief Close the connection; state transitions to @c Closed.
    virtual void close() = 0;
};

/// @brief Produces @ref IMediaConnection instances.
class IMediaConnectionFactory {
public:
    using StateChangedFn =
        std::function<void(StreamId, MediaConnectionState)>;

    virtual ~IMediaConnectionFactory() = default;

    /// @brief Install a state-change callback invoked for every
    /// connection produced by this factory.
    virtual void set_callback(StateChangedFn cb) = 0;

    /// @brief Open a new connection for @p params.
    virtual std::unique_ptr<IMediaConnection>
        open(const MediaConnectionParams& params) = 0;
};

}  // namespace xorio::orchestrator
