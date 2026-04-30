/// @file udp_socket_win32.cpp
/// @brief Winsock implementation of @ref UdpSocket.
///
/// Scope: socket primitives only. WSAStartup runs once via a
/// translation-unit-local guard so callers don't need to know
/// Winsock requires explicit init/shutdown.

#include "orchestrator/net/udp_socket.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace xorio::orchestrator::net {

namespace {

/// @brief One-shot WSAStartup. Held in a static so the cost is paid
/// at first socket open and not on every UdpSocket::open() call.
class WinsockInitGuard {
public:
    WinsockInitGuard() {
        WSADATA wsa{};
        ok_ = (::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }

    ~WinsockInitGuard() {
        if (ok_) ::WSACleanup();
    }

    WinsockInitGuard(const WinsockInitGuard&)            = delete;
    WinsockInitGuard& operator=(const WinsockInitGuard&) = delete;

    bool ok() const { return ok_; }

private:
    bool ok_ = false;
};

bool ensure_winsock() {
    static WinsockInitGuard g;
    return g.ok();
}

/// @brief Render the most recent WSA error code as a human string.
/// Caller passes the verb prefix; we append the formatted error.
std::string format_wsa_error(const std::string& what) {
    const int code = ::WSAGetLastError();
    return what + ": WSA error " + std::to_string(code);
}

class Win32UdpSocket final : public UdpSocket {
public:
    explicit Win32UdpSocket(SOCKET s) : sock_(s) {}

    ~Win32UdpSocket() override {
        if (sock_ != INVALID_SOCKET) ::closesocket(sock_);
    }

    bool set_reuse_addr(bool enable) override {
        const BOOL v = enable ? TRUE : FALSE;
        if (::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&v), sizeof(v))
            == SOCKET_ERROR) {
            last_error_ = format_wsa_error("setsockopt SO_REUSEADDR");
            return false;
        }
        last_error_.clear();
        return true;
    }

    bool set_broadcast(bool enable) override {
        const BOOL v = enable ? TRUE : FALSE;
        if (::setsockopt(sock_, SOL_SOCKET, SO_BROADCAST,
                         reinterpret_cast<const char*>(&v), sizeof(v))
            == SOCKET_ERROR) {
            last_error_ = format_wsa_error("setsockopt SO_BROADCAST");
            return false;
        }
        last_error_.clear();
        return true;
    }

    bool bind(const std::string& bind_ip, std::uint16_t port) override {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = ::htons(port);
        if (::inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
            last_error_ = "inet_pton: invalid IPv4 literal " + bind_ip;
            return false;
        }
        if (::bind(sock_, reinterpret_cast<const sockaddr*>(&addr),
                   sizeof(addr)) == SOCKET_ERROR) {
            last_error_ = format_wsa_error(
                "bind " + bind_ip + ":" + std::to_string(port));
            return false;
        }
        last_error_.clear();
        return true;
    }

    bool send_to(const std::uint8_t* bytes, std::size_t len,
                 const std::string& dest_ip,
                 std::uint16_t      dest_port) override {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = ::htons(dest_port);
        if (::inet_pton(AF_INET, dest_ip.c_str(), &addr.sin_addr) != 1) {
            last_error_ = "inet_pton: invalid dest " + dest_ip;
            return false;
        }
        const int n = ::sendto(sock_,
                               reinterpret_cast<const char*>(bytes),
                               static_cast<int>(len), 0,
                               reinterpret_cast<const sockaddr*>(&addr),
                               sizeof(addr));
        if (n == SOCKET_ERROR
            || static_cast<std::size_t>(n) != len) {
            last_error_ = format_wsa_error("sendto " + dest_ip);
            return false;
        }
        last_error_.clear();
        return true;
    }

    std::optional<ReceivedDatagram>
    recv_with_timeout(std::chrono::milliseconds timeout) override {
        WSAPOLLFD pfd{};
        pfd.fd     = sock_;
        pfd.events = POLLRDNORM;

        const int rc = ::WSAPoll(&pfd, 1, static_cast<int>(timeout.count()));
        if (rc == 0) {
            last_error_.clear();
            return std::nullopt;
        }
        if (rc == SOCKET_ERROR) {
            last_error_ = format_wsa_error("WSAPoll");
            return std::nullopt;
        }

        std::array<std::uint8_t, 65536> buf{};
        sockaddr_in src{};
        int         src_len = sizeof(src);
        const int n = ::recvfrom(sock_,
                                 reinterpret_cast<char*>(buf.data()),
                                 static_cast<int>(buf.size()), 0,
                                 reinterpret_cast<sockaddr*>(&src),
                                 &src_len);
        if (n == SOCKET_ERROR) {
            last_error_ = format_wsa_error("recvfrom");
            return std::nullopt;
        }

        ReceivedDatagram dg;
        dg.bytes.assign(buf.data(), buf.data() + n);
        char ip_buf[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &src.sin_addr,
                        ip_buf, sizeof(ip_buf)) != nullptr) {
            dg.source_ip = ip_buf;
        }
        dg.source_port = ::ntohs(src.sin_port);
        last_error_.clear();
        return dg;
    }

    std::string last_error() const override { return last_error_; }

private:
    SOCKET      sock_ = INVALID_SOCKET;
    std::string last_error_;
};

}  // namespace

std::unique_ptr<UdpSocket> UdpSocket::open() {
    if (!ensure_winsock()) return nullptr;
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return nullptr;
    return std::make_unique<Win32UdpSocket>(s);
}

}  // namespace xorio::orchestrator::net
