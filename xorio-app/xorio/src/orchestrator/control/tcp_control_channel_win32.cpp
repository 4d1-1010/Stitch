/// @file tcp_control_channel_win32.cpp
/// @brief Win32 (Winsock) implementation of @ref IControlChannel.
///
/// Scope: socket lifecycle + per-peer reader threads. Mirrors
/// `tcp_control_channel.cpp` (POSIX) function-for-function so the
/// orchestrator side never needs to know which platform is in
/// play. The frame codec lives in @ref protocol.cpp; this TU
/// only knows how to push / pull frames over a connected socket.
///
/// Threading:
///   * One accept thread services the listening socket.
///   * One reader thread per connected peer.
///   * send() is callable from any thread; per-peer mutex.
///
/// WSAStartup is called once on the first channel instance and
/// kept live for the process lifetime — Windows tears it down at
/// exit. WSACleanup on every dtor would race with other channels
/// on the same process if we ever build more than one.

#include "orchestrator/control/control_channel.hpp"
#include "orchestrator/control/protocol.hpp"

// NOMINMAX before <windows.h> so std::min/std::max stay usable.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xorio_ui::orchestrator::control {

namespace {

constexpr int  kAcceptTimeoutMs = 250;

/// @brief One-shot WSAStartup. Run on first instance creation;
/// the corresponding WSACleanup is intentionally omitted — the
/// process exit handles teardown.
void ensure_winsock_started() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA w;
        if (::WSAStartup(MAKEWORD(2, 2), &w) != 0) {
            std::fprintf(stderr, "control: WSAStartup failed\n");
        }
    });
}

bool read_exact(SOCKET fd, std::uint8_t* out, std::size_t n,
                const std::atomic<bool>& stop) {
    std::size_t got = 0;
    while (got < n) {
        if (stop.load(std::memory_order_acquire)) return false;
        const int r = ::recv(fd,
                             reinterpret_cast<char*>(out + got),
                             static_cast<int>(n - got), 0);
        if (r > 0) { got += static_cast<std::size_t>(r); continue; }
        if (r == 0) return false;          // peer closed.
        const int err = ::WSAGetLastError();
        if (err == WSAEINTR) continue;
        return false;
    }
    return true;
}

struct PeerLink {
    SOCKET            fd          = INVALID_SOCKET;
    std::string       peer_id;
    std::thread       reader;
    std::mutex        send_mtx;
    std::atomic<bool> stop{false};

    /// @brief When the peer closes its end mid-session, run_reader
    /// exits naturally and the reader thread is the *last* holder
    /// of this link's shared_ptr — so this destructor runs on the
    /// reader thread itself. Joining a thread from inside its own
    /// function deadlocks; destroying a joinable std::thread calls
    /// std::terminate. Detach in that case so the std::thread
    /// destructor is a clean no-op. The orderly-shutdown path
    /// (close_link → join) leaves @c reader non-joinable here.
    ~PeerLink() {
        if (reader.joinable()) {
            if (reader.get_id() == std::this_thread::get_id()) {
                reader.detach();
            } else {
                reader.join();
            }
        }
    }
};

class Win32ControlChannel final : public IControlChannel {
public:
    explicit Win32ControlChannel(std::string machine_id)
        : machine_id_(std::move(machine_id)) {
        ensure_winsock_started();
    }

    ~Win32ControlChannel() override { stop(); }

    bool start(std::uint16_t preferred_port) override {
        if (listen_fd_ != INVALID_SOCKET) return true;

        const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd == INVALID_SOCKET) {
            std::fprintf(stderr, "control: socket() failed (%d)\n",
                         ::WSAGetLastError());
            return false;
        }
        BOOL yes = TRUE;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(preferred_port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)) == SOCKET_ERROR) {
            std::fprintf(stderr, "control: bind(%u) failed (%d)\n",
                         preferred_port, ::WSAGetLastError());
            ::closesocket(fd);
            return false;
        }

        sockaddr_in bound{};
        int         bound_len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound),
                          &bound_len) == 0) {
            listen_port_ = ntohs(bound.sin_port);
        }

        if (::listen(fd, 8) == SOCKET_ERROR) {
            std::fprintf(stderr, "control: listen() failed (%d)\n",
                         ::WSAGetLastError());
            ::closesocket(fd);
            return false;
        }

        listen_fd_ = fd;
        accept_stop_.store(false, std::memory_order_release);
        accept_thread_ = std::thread(&Win32ControlChannel::run_accept, this);
        return true;
    }

    void stop() override {
        accept_stop_.store(true, std::memory_order_release);
        if (listen_fd_ != INVALID_SOCKET) {
            ::shutdown(listen_fd_, SD_BOTH);
            ::closesocket(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
        }
        if (accept_thread_.joinable()) accept_thread_.join();

        std::vector<std::shared_ptr<PeerLink>> to_drain;
        {
            std::lock_guard lk(peers_m_);
            to_drain.reserve(peers_.size());
            for (auto& [_, link] : peers_) to_drain.push_back(link);
            peers_.clear();
        }
        for (auto& link : to_drain) close_link(*link);
    }

    std::uint16_t listen_port() const override { return listen_port_; }

    void connect_to(const std::string& peer_machine_id,
                     const std::string& host,
                     std::uint16_t      port) override {
        if (peer_machine_id.empty() || peer_machine_id == machine_id_) return;
        {
            std::lock_guard lk(peers_m_);
            if (peers_.find(peer_machine_id) != peers_.end()) return;
        }

        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string port_s = std::to_string(port);
        if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0
            || res == nullptr) {
            std::fprintf(stderr, "control: getaddrinfo(%s:%u) failed\n",
                         host.c_str(), port);
            if (res) ::freeaddrinfo(res);
            return;
        }
        const SOCKET fd = ::socket(res->ai_family, res->ai_socktype,
                                    res->ai_protocol);
        if (fd == INVALID_SOCKET) {
            ::freeaddrinfo(res);
            return;
        }
        if (::connect(fd, res->ai_addr,
                       static_cast<int>(res->ai_addrlen)) == SOCKET_ERROR) {
            std::fprintf(stderr, "control: connect(%s:%u) failed (%d)\n",
                         host.c_str(), port, ::WSAGetLastError());
            ::closesocket(fd);
            ::freeaddrinfo(res);
            return;
        }
        ::freeaddrinfo(res);

        send_hello_locked(fd);

        auto link = std::make_shared<PeerLink>();
        link->fd      = fd;
        link->peer_id = peer_machine_id;
        register_link(link, /*we_initiated=*/true);
    }

    void disconnect(const std::string& peer_machine_id) override {
        std::shared_ptr<PeerLink> link;
        {
            std::lock_guard lk(peers_m_);
            auto it = peers_.find(peer_machine_id);
            if (it == peers_.end()) return;
            link = it->second;
            peers_.erase(it);
        }
        close_link(*link);
        if (on_disconnected_) on_disconnected_(peer_machine_id);
    }

    bool send(const std::string& peer_machine_id,
              MessageType type,
              const std::uint8_t* payload,
              std::size_t payload_len) override {
        std::shared_ptr<PeerLink> link;
        {
            std::lock_guard lk(peers_m_);
            auto it = peers_.find(peer_machine_id);
            if (it == peers_.end()) return false;
            link = it->second;
        }
        const auto frame = encode_frame(type, payload, payload_len);
        std::lock_guard sk(link->send_mtx);
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const int n = ::send(link->fd,
                                  reinterpret_cast<const char*>(frame.data() + sent),
                                  static_cast<int>(frame.size() - sent), 0);
            if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
            return false;
        }
        return true;
    }

    void set_callbacks(OnFrameFn     on_frame,
                        OnPeerEventFn on_connected,
                        OnPeerEventFn on_disconnected) override {
        on_frame_        = std::move(on_frame);
        on_connected_    = std::move(on_connected);
        on_disconnected_ = std::move(on_disconnected);
    }

private:
    void send_hello_locked(SOCKET fd) {
        HelloMessage h;
        h.machine_id       = machine_id_;
        h.protocol_version = kProtocolVersion;
        const auto body  = encode_hello(h);
        const auto frame = encode_frame(MessageType::Hello,
                                        body.data(), body.size());
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const int n = ::send(fd,
                                  reinterpret_cast<const char*>(frame.data() + sent),
                                  static_cast<int>(frame.size() - sent), 0);
            if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
            return;  // best-effort.
        }
    }

    void register_link(std::shared_ptr<PeerLink> link, bool we_initiated) {
        BOOL yes = TRUE;
        ::setsockopt(link->fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));

        link->reader = std::thread(
            &Win32ControlChannel::run_reader, this, link, we_initiated);

        std::lock_guard lk(peers_m_);
        peers_[link->peer_id] = std::move(link);
    }

    void close_link(PeerLink& link) {
        link.stop.store(true, std::memory_order_release);
        if (link.fd != INVALID_SOCKET) {
            ::shutdown(link.fd, SD_BOTH);
            ::closesocket(link.fd);
            link.fd = INVALID_SOCKET;
        }
        if (link.reader.joinable()) link.reader.join();
    }

    void run_accept() {
        while (!accept_stop_.load(std::memory_order_acquire)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_fd_, &rfds);
            timeval tv{ 0, kAcceptTimeoutMs * 1000 };
            const int r = ::select(0, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            if (!FD_ISSET(listen_fd_, &rfds)) continue;

            sockaddr_in peer_addr{};
            int         peer_len = sizeof(peer_addr);
            const SOCKET fd = ::accept(
                listen_fd_, reinterpret_cast<sockaddr*>(&peer_addr),
                &peer_len);
            if (fd == INVALID_SOCKET) continue;

            auto link = std::make_shared<PeerLink>();
            link->fd = fd;
            link->reader = std::thread(
                &Win32ControlChannel::run_reader, this, link,
                /*we_initiated=*/false);
        }
    }

    void run_reader(std::shared_ptr<PeerLink> link, bool we_initiated) {
        if (!we_initiated) {
            std::lock_guard sk(link->send_mtx);
            send_hello_locked(link->fd);
        }

        bool greeted = false;
        while (!link->stop.load(std::memory_order_acquire)) {
            std::uint8_t header[kFrameHeaderSize];
            if (!read_exact(link->fd, header, kFrameHeaderSize, link->stop)) {
                break;
            }
            const auto h = decode_frame_header(header, kFrameHeaderSize);
            if (!h) break;

            std::vector<std::uint8_t> payload;
            payload.resize(h->payload_len);
            if (h->payload_len > 0
                && !read_exact(link->fd, payload.data(),
                                payload.size(), link->stop)) {
                break;
            }

            if (!greeted) {
                if (h->type != MessageType::Hello) break;
                auto hello = decode_hello(payload.data(), payload.size());
                if (!hello || hello->machine_id.empty()) break;
                greeted = true;

                if (we_initiated) {
                    if (on_connected_) on_connected_(link->peer_id);
                } else {
                    link->peer_id = hello->machine_id;
                    {
                        std::lock_guard lk(peers_m_);
                        auto it = peers_.find(link->peer_id);
                        if (it != peers_.end() && it->second.get() != link.get()) {
                            return;  // duplicate inbound; drop.
                        }
                        peers_[link->peer_id] = link;
                    }
                    BOOL yes = TRUE;
                    ::setsockopt(link->fd, IPPROTO_TCP, TCP_NODELAY,
                                 reinterpret_cast<const char*>(&yes), sizeof(yes));
                    if (on_connected_) on_connected_(link->peer_id);
                }
                continue;
            }

            if (on_frame_) {
                InboundFrame f;
                f.type    = h->type;
                f.payload = std::move(payload);
                on_frame_(link->peer_id, f);
            }
        }

        const std::string peer_id = link->peer_id;
        bool was_live = false;
        if (!peer_id.empty()) {
            std::lock_guard lk(peers_m_);
            auto it = peers_.find(peer_id);
            if (it != peers_.end() && it->second.get() == link.get()) {
                peers_.erase(it);
                was_live = true;
            }
        }
        if (link->fd != INVALID_SOCKET) {
            ::shutdown(link->fd, SD_BOTH);
            ::closesocket(link->fd);
            link->fd = INVALID_SOCKET;
        }
        if (was_live && on_disconnected_) on_disconnected_(peer_id);
    }

    std::string                                                machine_id_;
    SOCKET                                                     listen_fd_   = INVALID_SOCKET;
    std::uint16_t                                              listen_port_ = 0;
    std::atomic<bool>                                          accept_stop_{false};
    std::thread                                                accept_thread_;

    mutable std::mutex                                         peers_m_;
    std::unordered_map<std::string, std::shared_ptr<PeerLink>> peers_;

    OnFrameFn      on_frame_;
    OnPeerEventFn  on_connected_;
    OnPeerEventFn  on_disconnected_;
};

}  // namespace

std::unique_ptr<IControlChannel>
make_tcp_control_channel(const std::string& machine_id) {
    return std::make_unique<Win32ControlChannel>(machine_id);
}

}  // namespace xorio_ui::orchestrator::control
