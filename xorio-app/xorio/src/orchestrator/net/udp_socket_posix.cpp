/// @file udp_socket_posix.cpp
/// @brief BSD-socket implementation of @ref UdpSocket on Linux.
///
/// Scope: socket primitives only. Higher layers in
/// `lan_discovery.cpp` own threading, peer state, and the announce
/// loop.

#include "orchestrator/net/udp_socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>

namespace xorio::orchestrator::net {

namespace {

class PosixUdpSocket final : public UdpSocket {
public:
    explicit PosixUdpSocket(int fd) : fd_(fd) {}

    ~PosixUdpSocket() override {
        if (fd_ >= 0) ::close(fd_);
    }

    bool set_reuse_addr(bool enable) override {
        const int v = enable ? 1 : 0;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR,
                         &v, sizeof(v)) != 0) {
            capture_errno("setsockopt SO_REUSEADDR");
            return false;
        }
        last_error_.clear();
        return true;
    }

    bool set_broadcast(bool enable) override {
        const int v = enable ? 1 : 0;
        if (::setsockopt(fd_, SOL_SOCKET, SO_BROADCAST,
                         &v, sizeof(v)) != 0) {
            capture_errno("setsockopt SO_BROADCAST");
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
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr),
                   sizeof(addr)) != 0) {
            capture_errno("bind " + bind_ip + ":"
                          + std::to_string(port));
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
        const ssize_t n = ::sendto(fd_, bytes, len, 0,
                                   reinterpret_cast<const sockaddr*>(&addr),
                                   sizeof(addr));
        if (n < 0 || static_cast<std::size_t>(n) != len) {
            capture_errno("sendto " + dest_ip);
            return false;
        }
        last_error_.clear();
        return true;
    }

    std::optional<ReceivedDatagram>
    recv_with_timeout(std::chrono::milliseconds timeout) override {
        pollfd pfd{};
        pfd.fd     = fd_;
        pfd.events = POLLIN;

        const int rc = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        if (rc == 0) {
            last_error_.clear();
            return std::nullopt;        // timeout
        }
        if (rc < 0) {
            if (errno == EINTR) {
                last_error_.clear();
                return std::nullopt;    // signal interruption is benign
            }
            capture_errno("poll");
            return std::nullopt;
        }

        std::array<std::uint8_t, 65536> buf{};
        sockaddr_in src{};
        socklen_t   src_len = sizeof(src);
        const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0,
                                     reinterpret_cast<sockaddr*>(&src),
                                     &src_len);
        if (n < 0) {
            capture_errno("recvfrom");
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
    void capture_errno(const std::string& what) {
        last_error_ = what + ": " + std::strerror(errno);
    }

    int         fd_ = -1;
    std::string last_error_;
};

}  // namespace

std::unique_ptr<UdpSocket> UdpSocket::open() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return nullptr;
    return std::make_unique<PosixUdpSocket>(fd);
}

}  // namespace xorio::orchestrator::net
