/// @file lan_interfaces_win32.cpp
/// @brief Windows IPv4 interface enumeration via
/// GetAdaptersAddresses.
///
/// Scope: enumerate + filter only — same shape as the POSIX twin.

#include "orchestrator/net/lan_interfaces.hpp"

#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <cstdint>
#include <vector>

namespace xorio::orchestrator::net {

namespace {

/// @brief Format an IPv4 sockaddr as a dotted-decimal string.
std::string format_ipv4(const sockaddr_in* sa) {
    if (sa == nullptr) return {};
    char buf[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &sa->sin_addr,
                    buf, sizeof(buf)) == nullptr) return {};
    return buf;
}

/// @brief Compute subnet broadcast from an IPv4 address + prefix
/// length (e.g. 24 for /24). Result is dotted-decimal.
std::string compute_broadcast(const sockaddr_in* sa, std::uint8_t prefix) {
    if (sa == nullptr || prefix > 32) return {};
    const std::uint32_t ip   = ::ntohl(sa->sin_addr.s_addr);
    const std::uint32_t mask = (prefix == 0)
                               ? 0u
                               : (~0u << (32 - prefix));
    const std::uint32_t bcast_h = (ip & mask) | (~mask);

    in_addr bcast{};
    bcast.s_addr = ::htonl(bcast_h);
    char buf[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &bcast, buf, sizeof(buf)) == nullptr) return {};
    return buf;
}

bool is_loopback(const sockaddr_in* sa) {
    if (sa == nullptr) return false;
    return (::ntohl(sa->sin_addr.s_addr) >> 24) == 127u;
}

std::string narrow(const wchar_t* w) {
    if (w == nullptr) return {};
    std::string out;
    while (*w) {
        // Adapter friendly names are ASCII in practice; non-ASCII
        // glyphs become '?' here, which is a display-only fallback
        // for the rare exotic adapter name. Discovery itself uses
        // the IP, not the name.
        out.push_back(*w < 128 ? static_cast<char>(*w) : '?');
        ++w;
    }
    return out;
}

}  // namespace

std::vector<LanInterface> enumerate_lan_interfaces() {
    std::vector<LanInterface> out;

    // Two-pass call — first to size the buffer, then to fill it.
    // Sticking to the documented retry-on-grow loop because the
    // adapter set can change between the two calls on a busy host.
    ULONG buf_len = 16 * 1024;
    std::vector<std::uint8_t> buffer(buf_len);
    constexpr ULONG kFlags =
        GAA_FLAG_SKIP_ANYCAST  |
        GAA_FLAG_SKIP_MULTICAST |
        GAA_FLAG_SKIP_DNS_SERVER;

    ULONG rc = ::GetAdaptersAddresses(
        AF_INET, kFlags, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
        &buf_len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buf_len);
        rc = ::GetAdaptersAddresses(
            AF_INET, kFlags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &buf_len);
    }
    if (rc != NO_ERROR) return out;

    auto* adapter =
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    for (; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (auto* uni = adapter->FirstUnicastAddress;
             uni != nullptr; uni = uni->Next) {
            if (uni->Address.lpSockaddr == nullptr) continue;
            if (uni->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto* sa =
                reinterpret_cast<const sockaddr_in*>(uni->Address.lpSockaddr);
            if (is_loopback(sa)) continue;

            LanInterface entry;
            entry.name = narrow(adapter->FriendlyName);
            entry.ip   = format_ipv4(sa);
            entry.broadcast_ip =
                compute_broadcast(sa, uni->OnLinkPrefixLength);
            if (!entry.ip.empty()) out.push_back(std::move(entry));
        }
    }
    return out;
}

}  // namespace xorio::orchestrator::net
