/// @file lan_interfaces_posix.cpp
/// @brief Linux IPv4 interface enumeration via getifaddrs(3).
///
/// Scope: enumerate + filter only. The discovery loop owns
/// re-enumeration cadence + stable-id tracking.

#include "orchestrator/net/lan_interfaces.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>

namespace xorio::orchestrator::net {

namespace {

/// @brief Format an `in_addr` as a dotted-decimal string. Empty on
/// error.
std::string format_ipv4(const in_addr& addr) {
    char buf[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &addr, buf, sizeof(buf)) == nullptr) return {};
    return buf;
}

/// @brief Compute the subnet broadcast for @p ip + @p netmask. The
/// kernel-reported `ifa_broadaddr` is preferred; this is the
/// fallback when an interface omits it (some VPN / tunnel NICs).
std::string compute_broadcast(const in_addr& ip, const in_addr& netmask) {
    in_addr bcast{};
    bcast.s_addr = (ip.s_addr & netmask.s_addr) | (~netmask.s_addr);
    return format_ipv4(bcast);
}

bool is_loopback_or_unspecified(const in_addr& addr) {
    const std::uint32_t a = ::ntohl(addr.s_addr);
    if ((a >> 24) == 127)  return true;   // 127.0.0.0/8
    if (a == 0)            return true;   // 0.0.0.0
    return false;
}

}  // namespace

std::vector<LanInterface> enumerate_lan_interfaces() {
    std::vector<LanInterface> out;

    ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0 || head == nullptr) return out;

    for (ifaddrs* it = head; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr)               continue;
        if (it->ifa_addr->sa_family != AF_INET)    continue;
        if ((it->ifa_flags & IFF_UP)       == 0)   continue;
        if ((it->ifa_flags & IFF_RUNNING)  == 0)   continue;

        const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
        if (is_loopback_or_unspecified(sin->sin_addr)) continue;

        LanInterface entry;
        entry.name = it->ifa_name ? it->ifa_name : "";
        entry.ip   = format_ipv4(sin->sin_addr);

        if (it->ifa_broadaddr != nullptr
            && it->ifa_broadaddr->sa_family == AF_INET) {
            const auto* sb =
                reinterpret_cast<const sockaddr_in*>(it->ifa_broadaddr);
            entry.broadcast_ip = format_ipv4(sb->sin_addr);
        } else if (it->ifa_netmask != nullptr
                   && it->ifa_netmask->sa_family == AF_INET) {
            const auto* sm =
                reinterpret_cast<const sockaddr_in*>(it->ifa_netmask);
            entry.broadcast_ip = compute_broadcast(sin->sin_addr,
                                                   sm->sin_addr);
        }

        if (!entry.ip.empty()) out.push_back(std::move(entry));
    }

    ::freeifaddrs(head);
    return out;
}

}  // namespace xorio::orchestrator::net
