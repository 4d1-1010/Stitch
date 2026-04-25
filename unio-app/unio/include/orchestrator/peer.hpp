/// @file peer.hpp
/// @brief Peer identity + connectivity snapshot.
#pragma once

#include <string>

namespace unio_ui::orchestrator {

/// @brief A machine participating in the local mesh.
///
/// Mesh CRDT records identify the owner by @ref Peer::machine_id.
/// Reachability metadata (@ref online, @ref address, ...) is
/// derived from the control connection's heartbeat stream.
struct Peer {
    std::string  machine_id;       ///< Globally unique pairing-pubkey fingerprint.
    std::string  display_name;     ///< Human caption; user-editable on the owning PC.
    std::string  address;          ///< Primary LAN IPv4 or IPv6 literal.
    bool         paired   = false; ///< Trust established (pairing key exchanged).
    bool         online   = true;  ///< Heartbeat seen within the staleness threshold.
    bool         is_local = false; ///< @c true for the machine running this process.
};

}  // namespace unio_ui::orchestrator
