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

namespace xorio::orchestrator::net {

/// @brief Construction parameters surfaced by the announce loop.
struct LanDiscoveryConfig {
    std::string                machine_id;     ///< Stable id; advertised verbatim.
    std::string                hostname;       ///< Display name; advertised verbatim.
    std::uint16_t              tcp_port  = 0;  ///< Mesh control port; advertised verbatim.
    /// @brief Data-channel port (file-transfer TCP). Advertised
    /// alongside @c tcp_port so peers can target file bytes at
    /// a separate listen socket from cursor / keyboard control.
    std::uint16_t              data_port = 0;

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

    /// @brief Live source of the local workspace catalogue
    /// (including tombstones). Called once per announce tick so
    /// every workspace mutation rides the next datagram. Empty /
    /// missing = the announce omits the workspaces extension.
    std::function<std::vector<AnnounceWorkspace>()> workspaces_provider;

    /// @brief Live source of the local "Identify" counter. Read
    /// every announce tick + advertised on the wire so peers see
    /// the bump within ~one announce interval.
    const std::atomic<std::uint64_t>*  identify_counter = nullptr;

    /// @brief Fired when a remote peer's identify counter bumps
    /// (i.e. that peer's user clicked Identify). Receivers run
    /// their own local Identify overlay in response so every
    /// PC's monitors flash at once. The argument is the remote
    /// peer's machine_id; ignored if `nullptr`.
    std::function<void(const std::string& peer_machine_id)>
        on_peer_identify;
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

}  // namespace xorio::orchestrator::net
