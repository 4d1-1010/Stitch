/// @file control_channel.hpp
/// @brief Peer-to-peer control-channel transport interface.
///
/// Scope: abstract façade over whatever socket layer the platform
/// provides. The Linux POSIX implementation lives in
/// `tcp_control_channel.cpp` and is the only impl in Phase A; a
/// future Windows port slots in next to it without touching the
/// callers.
///
/// Connection model:
///   * One process listens on a single TCP port (chosen at start)
///     and accepts inbound connections from peers.
///   * Outbound connections are made on demand via @ref connect_to,
///     typically once the orchestrator observes a peer over LAN
///     discovery and learns its advertised port.
///   * Each peer is identified by its machine_id; the channel
///     deduplicates so two simultaneous attempts (one in each
///     direction) collapse into one live connection.
///
/// Thread model:
///   * The channel owns its own background threads (one accept
///     loop + one read loop per connected peer).
///   * Callbacks fire from those threads; receivers should marshal
///     onto a UI / orchestrator thread before mutating shared
///     state.
#pragma once

#include "orchestrator/control/protocol.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace unio_ui::orchestrator::control {

/// @brief One frame as delivered to the on-message callback. Keeps
/// the type + raw payload bytes — the receiver picks the right
/// per-message decoder (`decode_hello`, `decode_mouse_move`, …).
struct InboundFrame {
    MessageType                type;
    std::vector<std::uint8_t>  payload;
};

/// @brief Peer-to-peer control channel. Linux TCP today; Windows
/// later.
class IControlChannel {
public:
    /// @brief Fired when @p peer_id sends a frame. Runs on the
    /// channel's read thread — do as little as possible inline.
    using OnFrameFn = std::function<void(const std::string& peer_id,
                                          const InboundFrame&)>;
    /// @brief Fired right after the Hello handshake completes for
    /// @p peer_id (either inbound or outbound). The channel
    /// guarantees a matching disconnected callback.
    using OnPeerEventFn = std::function<void(const std::string& peer_id)>;

    virtual ~IControlChannel() = default;

    /// @brief Open the listening socket. @p preferred_port is a
    /// hint; pass 0 to let the kernel pick. Returns true on
    /// success; @ref listen_port reports the actual bound port.
    virtual bool start(std::uint16_t preferred_port) = 0;

    /// @brief Close every connection + the listening socket.
    virtual void stop() = 0;

    /// @brief Bound port (valid only after a successful start).
    virtual std::uint16_t listen_port() const = 0;

    /// @brief Open an outbound TCP connection to @p host:@p port
    /// and exchange Hello messages. No-op if a live connection to
    /// @p peer_machine_id already exists.
    virtual void connect_to(const std::string& peer_machine_id,
                             const std::string& host,
                             std::uint16_t      port) = 0;

    /// @brief Drop the connection to @p peer_machine_id (if any).
    virtual void disconnect(const std::string& peer_machine_id) = 0;

    /// @brief Send @p type + @p payload to @p peer_machine_id.
    /// Returns false if no live connection exists.
    virtual bool send(const std::string& peer_machine_id,
                       MessageType type,
                       const std::uint8_t* payload,
                       std::size_t payload_len) = 0;

    /// @brief Wire callbacks. Replaces any previously-registered
    /// callbacks. Pass nullptr to clear.
    virtual void set_callbacks(OnFrameFn     on_frame,
                                OnPeerEventFn on_connected,
                                OnPeerEventFn on_disconnected) = 0;
};

/// @brief Build the Linux POSIX TCP implementation. @p machine_id
/// is sent in the Hello handshake so the remote side learns who
/// we are.
std::unique_ptr<IControlChannel>
make_tcp_control_channel(const std::string& machine_id);

}  // namespace unio_ui::orchestrator::control
