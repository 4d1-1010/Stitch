/// @file control_connection.hpp
/// @brief Always-on QUIC connection per paired peer: CRDT +
/// heartbeats + RPC over independent streams.
#pragma once

#include "orchestrator/mesh_crdt.hpp"
#include "orchestrator/stream.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xorio_ui::orchestrator {

/// @brief Stream-IDs multiplexed within one control connection.
enum class ControlStream {
    Caps,
    Layout,
    Workspaces,
    Clipboard,
    Rpc,
    Presence,
};

/// @brief RPC kinds dispatched over the @c Rpc stream.
enum class RpcKind {
    PrepareMedia,   ///< Orchestrator → remote orchestrator before StartStream.
    StopMedia,      ///< Orchestrator → remote orchestrator on StopStream.
    RequestIdr,     ///< Sink → source request for a fresh key frame.
};

/// @brief One RPC message exchanged on the Rpc stream.
struct RpcMessage {
    RpcKind                    kind;
    StreamId                   stream_id;
    std::vector<std::uint8_t>  body;   ///< Serialized codec-negotiation payload.
};

/// @brief Per-peer always-on control connection.
///
/// Produced by @ref IControlConnectionManager when a peer pairs
/// or comes online. Owned by the orchestrator for the lifetime
/// of the pairing. Reconnects transparently across brief network
/// blips; ownership outlives any transient disconnect.
class IControlConnection {
public:
    virtual ~IControlConnection() = default;

    /// @brief Peer this connection talks to.
    virtual std::string peer_machine_id() const = 0;

    /// @brief @c true once the first handshake completes.
    virtual bool is_established() const = 0;

    /// @brief Publish @p bytes on one of the CRDT streams.
    /// The caller is responsible for signing the bytes.
    virtual void publish(ControlStream slot,
                         const std::uint8_t* bytes,
                         std::size_t len) = 0;

    /// @brief Send an RPC request. Response (if any) surfaces via
    /// the callback installed by @ref set_rpc_handler.
    virtual void send_rpc(const RpcMessage& msg) = 0;

    /// @brief Register handlers for incoming streams.
    ///
    /// The CRDT bytes handler is invoked with the raw signed
    /// payload; the caller passes the bytes to
    /// @c IMeshCrdt::merge_signed_bytes. The RPC handler receives
    /// incoming RPC messages.
    virtual void set_handlers(
        std::function<void(Slot, const std::uint8_t*, std::size_t)> on_crdt_bytes,
        std::function<void(const RpcMessage&)> on_rpc) = 0;
};

/// @brief Factory + lifecycle owner for @ref IControlConnection instances.
class IControlConnectionManager {
public:
    using OpenedFn = std::function<void(IControlConnection&)>;
    using ClosedFn = std::function<void(const std::string& machine_id)>;

    virtual ~IControlConnectionManager() = default;

    /// @brief Register lifecycle callbacks.
    virtual void set_callbacks(OpenedFn on_opened, ClosedFn on_closed) = 0;

    /// @brief Open (or reuse) the control connection to @p machine_id.
    virtual void ensure_connection(const std::string& machine_id,
                                   const std::string& address,
                                   std::uint16_t port) = 0;

    /// @brief Tear down the connection to @p machine_id.
    virtual void close_connection(const std::string& machine_id) = 0;

    /// @brief Enumerate currently-established connections.
    virtual std::vector<std::string> connected_peers() const = 0;
};

}  // namespace xorio_ui::orchestrator
