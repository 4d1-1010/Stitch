/// @file lan_interfaces.hpp
/// @brief Enumerate the host's IPv4 LAN interfaces.
///
/// Scope: report each non-loopback IPv4 NIC's address + computed
/// subnet broadcast. No socket I/O, no caching across calls — the
/// caller decides how often to re-enumerate (interface state can
/// change while the app is running, e.g. cable plug/unplug).
///
/// Per-OS impls live in
/// `src/orchestrator/net/lan_interfaces_{posix,win32}.cpp`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xorio::orchestrator::net {

/// @brief One enumerated IPv4 interface.
struct LanInterface {
    std::string  name;          ///< OS-reported NIC name (eth0, Ethernet, …).
    std::string  ip;             ///< Primary IPv4 address as a literal.
    std::string  broadcast_ip;   ///< Subnet broadcast (e.g. 192.168.1.255).
};

/// @brief Enumerate every non-loopback IPv4 interface on the host.
///
/// Loopback (`127.0.0.0/8`), link-local self-assigned ranges, and
/// virtual interfaces with no broadcast address are filtered out
/// before return so the caller can iterate without checks.
///
/// On Linux uses `getifaddrs(3)`; on Windows uses
/// `GetAdaptersAddresses`. Returns an empty vector if the OS call
/// fails — the discovery layer falls back to `255.255.255.255` on
/// the default route.
std::vector<LanInterface> enumerate_lan_interfaces();

}  // namespace xorio::orchestrator::net
