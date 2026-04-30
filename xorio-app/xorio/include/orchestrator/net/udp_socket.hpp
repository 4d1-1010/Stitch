/// @file udp_socket.hpp
/// @brief Thin platform-neutral UDP socket. POSIX + Winsock impls
/// live in `src/orchestrator/net/udp_socket_{posix,win32}.cpp`.
///
/// Scope: open / bind / send / recv only. No protocol parsing, no
/// peer tracking, no threading. The wire format is owned by
/// `announce_codec.hpp`; the loop that ticks announces + collects
/// inbound is owned by `lan_discovery.hpp`.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xorio_ui::orchestrator::net {

/// @brief A single received datagram.
struct ReceivedDatagram {
    std::vector<std::uint8_t> bytes;        ///< Payload (truncated to 64 KiB).
    std::string               source_ip;    ///< Sender's IPv4 literal.
    std::uint16_t             source_port = 0;
};

/// @brief Owned IPv4 UDP socket.
///
/// Move-only. Destructor closes the underlying handle. No
/// exceptions on the hot path — failures surface through the bool
/// return values + the static @ref last_error() string for human
/// reporting.
class UdpSocket {
public:
    /// @brief Construct an unbound socket. Returns an empty
    /// `unique_ptr` if the OS rejects the create call.
    static std::unique_ptr<UdpSocket> open();

    virtual ~UdpSocket() = default;

    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&&)                 = default;
    UdpSocket& operator=(UdpSocket&&)      = default;

    /// @brief Allow the kernel to permit a second bind on the same
    /// (addr, port) — needed for the listen socket so a previous
    /// crash's lingering socket doesn't trip EADDRINUSE.
    virtual bool set_reuse_addr(bool enable) = 0;

    /// @brief Permit sendto() with an IPv4 broadcast destination.
    virtual bool set_broadcast(bool enable) = 0;

    /// @brief Bind to (`bind_ip`, `port`). Pass "0.0.0.0" to listen
    /// on every interface; pass an interface IP to scope outbound
    /// broadcasts to that NIC.
    virtual bool bind(const std::string& bind_ip,
                      std::uint16_t      port) = 0;

    /// @brief Send @p bytes to (`dest_ip`, `dest_port`).
    /// @return @c true iff the kernel accepted the datagram for
    /// transmission. Lower-level transit is not acknowledged.
    virtual bool send_to(const std::uint8_t* bytes, std::size_t len,
                         const std::string& dest_ip,
                         std::uint16_t      dest_port) = 0;

    /// @brief Block until a datagram arrives or @p timeout elapses.
    /// @return The datagram on success, empty optional on timeout.
    /// On error, returns empty + sets @ref last_error().
    virtual std::optional<ReceivedDatagram>
    recv_with_timeout(std::chrono::milliseconds timeout) = 0;

    /// @brief Per-thread human-readable description of the most
    /// recent failure on this socket. Cleared on every successful
    /// op. Empty string if the last op succeeded.
    virtual std::string last_error() const = 0;

protected:
    UdpSocket() = default;
};

}  // namespace xorio_ui::orchestrator::net
