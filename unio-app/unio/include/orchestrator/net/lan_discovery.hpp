/// @file lan_discovery.hpp
/// @brief Real LAN-discovery service: replaces the mock with a
/// UDP broadcast / listen loop that produces live peer events.
///
/// Scope: factory only. The implementation in
/// `src/orchestrator/net/lan_discovery.cpp` orchestrates
/// `UdpSocket`, `enumerate_lan_interfaces()`, and the announce
/// codec; spawns one background thread that ticks announces every
/// 2 s, drains incoming datagrams, and sweeps stale peers past a
/// 10 s TTL.
///
/// Wire compatibility with the Python tree is enforced by
/// `announce_codec.hpp`'s schema.
#pragma once

#include "orchestrator/discovery.hpp"
#include "orchestrator/net/announce_codec.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace unio_ui::orchestrator::net {

/// @brief Construction parameters surfaced by the announce loop.
struct LanDiscoveryConfig {
    std::string                machine_id;     ///< Stable id; advertised verbatim.
    std::string                hostname;       ///< Display name; advertised verbatim.
    std::uint16_t              tcp_port = 0;   ///< Mesh control port; advertised verbatim.

    /// @brief Live "this peer is authorized" flag. Read from the
    /// announce loop on every tick so flips are picked up without
    /// restarting discovery; surfaces on the wire as `authed:true`
    /// when the local user has signed in (typed the access-gate
    /// key). Listeners use the bit on incoming announces to
    /// auto-authorize themselves — see the orchestrator façade.
    /// @c nullptr = always advertise unauthorized.
    const std::atomic<bool>*   authed_flag = nullptr;

    /// @brief Live source of the local probe's display geometry.
    /// Called once per announce tick so reconfigurations (cable
    /// plug/unplug, resolution change) ride the next datagram.
    /// Empty / missing = the announce omits the displays
    /// extension entirely (older Python instances do this).
    std::function<std::vector<AnnounceDisplay>()> displays_provider;
};

/// @brief Build a real LAN-discovery service.
///
/// The returned object behaves exactly like @ref IDiscovery's
/// contract: callers use @c start() / @c stop() and receive
/// per-peer events through the supplied callbacks. Failures during
/// socket setup are reported via stderr (logging subsystem lands
/// later) and the loop continues with whichever broadcast sockets
/// did open — partial coverage is better than no announces.
std::unique_ptr<IDiscovery>
make_lan_discovery(const LanDiscoveryConfig& config);

}  // namespace unio_ui::orchestrator::net
