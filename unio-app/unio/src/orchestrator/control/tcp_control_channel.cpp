/// @file tcp_control_channel.cpp
/// @brief POSIX TCP implementation of @ref IControlChannel.
///
/// Scope: socket lifecycle + per-peer reader threads. Pure socket
/// code — no orchestrator or UI deps. The frame codec lives in
/// @ref protocol.cpp; this file only knows how to push / pull
/// frames over a connected socket.
///
/// Threading:
///   * One accept thread services the listening socket.
///   * One reader thread per connected peer — drains frames until
///     EOF or socket error and dispatches them via on_frame_.
///   * send() is called from any thread and grabs the per-peer
///     mutex while writing.
///
/// Linux-only build target. The CMakeLists guard limits compilation
/// of this TU to non-Win32 platforms; a Windows port will land as a
/// sibling tcp_control_channel_win32.cpp later.

#include "orchestrator/control/control_channel.hpp"
#include "orchestrator/control/protocol.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace unio_ui::orchestrator::control {

namespace {

constexpr int  kInvalidFd     = -1;
constexpr int  kAcceptTimeout = 250;   ///< ms — poll loop wakeups.

/// @brief Read exactly @p n bytes from @p fd into @p out. Returns
/// false on EOF or socket error.
bool read_exact(int fd, std::uint8_t* out, std::size_t n,
                const std::atomic<bool>& stop) {
    std::size_t got = 0;
    while (got < n) {
        if (stop.load(std::memory_order_acquire)) return false;
        const ssize_t r = ::recv(fd, out + got, n - got, 0);
        if (r > 0) { got += static_cast<std::size_t>(r); continue; }
        if (r == 0) return false;          // peer closed.
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

/// @brief Per-connection state.
struct PeerLink {
    int                            fd          = kInvalidFd;
    std::string                    peer_id;     ///< filled by Hello handshake.
    std::thread                    reader;
    std::mutex                     send_mtx;
    std::atomic<bool>              stop{false};

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

class TcpControlChannel final : public IControlChannel {
public:
    explicit TcpControlChannel(std::string machine_id)
        : machine_id_(std::move(machine_id)) {}

    ~TcpControlChannel() override { stop(); }

    bool start(std::uint16_t preferred_port) override {
        if (listen_fd_ != kInvalidFd) return true;  // already started.

        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            std::fprintf(stderr, "control: socket() failed: %s\n",
                         std::strerror(errno));
            return false;
        }
        int yes = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(preferred_port);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::fprintf(stderr, "control: bind(%u) failed: %s\n",
                         preferred_port, std::strerror(errno));
            ::close(fd);
            return false;
        }

        sockaddr_in bound{};
        socklen_t   bound_len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound),
                          &bound_len) == 0) {
            listen_port_ = ntohs(bound.sin_port);
        }

        if (::listen(fd, 8) < 0) {
            std::fprintf(stderr, "control: listen() failed: %s\n",
                         std::strerror(errno));
            ::close(fd);
            return false;
        }

        listen_fd_ = fd;
        accept_stop_.store(false, std::memory_order_release);
        accept_thread_ = std::thread(&TcpControlChannel::run_accept, this);
        return true;
    }

    void stop() override {
        accept_stop_.store(true, std::memory_order_release);
        if (listen_fd_ != kInvalidFd) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = kInvalidFd;
        }
        if (accept_thread_.joinable()) accept_thread_.join();

        // Drain peers. Take the map under the lock, clear, then
        // close + join outside the lock so a reader's callback
        // doesn't deadlock against us.
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

        // Resolve + connect synchronously. Connections are rare
        // (one per peer) so a brief block here is fine.
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
        const int fd = ::socket(res->ai_family, res->ai_socktype,
                                res->ai_protocol);
        if (fd < 0) {
            ::freeaddrinfo(res);
            return;
        }
        if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
            std::fprintf(stderr, "control: connect(%s:%u) failed: %s\n",
                         host.c_str(), port, std::strerror(errno));
            ::close(fd);
            ::freeaddrinfo(res);
            return;
        }
        ::freeaddrinfo(res);

        // Send Hello first (we initiated). The peer's reader will
        // greet us back with their own Hello; we wire up the link
        // optimistically using @p peer_machine_id, then the
        // handshake confirms or replaces it.
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
            const ssize_t n = ::send(link->fd, frame.data() + sent,
                                     frame.size() - sent, MSG_NOSIGNAL);
            if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
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
    /// @brief Send our Hello over @p fd. Used both by inbound (the
    /// accept thread) and outbound (connect_to) paths so each side
    /// announces itself exactly once.
    void send_hello_locked(int fd) {
        HelloMessage h;
        h.machine_id       = machine_id_;
        h.protocol_version = kProtocolVersion;
        const auto body  = encode_hello(h);
        const auto frame = encode_frame(MessageType::Hello,
                                        body.data(), body.size());
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const ssize_t n = ::send(fd, frame.data() + sent,
                                     frame.size() - sent, MSG_NOSIGNAL);
            if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            return;  // best-effort.
        }
    }

    /// @brief Insert @p link into the peer map and spawn its
    /// reader thread. If @p we_initiated is true we already sent
    /// Hello; otherwise the reader's first job is to send our
    /// Hello in response to the inbound one.
    void register_link(std::shared_ptr<PeerLink> link, bool we_initiated) {
        // Disable Nagle so single mouse-move frames don't get
        // batched up to 40 ms — this is a low-latency control
        // channel.
        int yes = 1;
        ::setsockopt(link->fd, IPPROTO_TCP, TCP_NODELAY,
                     &yes, sizeof(yes));

        link->reader = std::thread(
            &TcpControlChannel::run_reader, this, link, we_initiated);

        std::lock_guard lk(peers_m_);
        peers_[link->peer_id] = std::move(link);
    }

    void close_link(PeerLink& link) {
        link.stop.store(true, std::memory_order_release);
        if (link.fd != kInvalidFd) {
            ::shutdown(link.fd, SHUT_RDWR);
            ::close(link.fd);
            link.fd = kInvalidFd;
        }
        if (link.reader.joinable()) link.reader.join();
    }

    /// @brief Accept-loop body. Each new socket spawns a reader
    /// that completes the handshake before the link gets
    /// registered under the peer's machine_id.
    void run_accept() {
        while (!accept_stop_.load(std::memory_order_acquire)) {
            pollfd p{listen_fd_, POLLIN, 0};
            const int r = ::poll(&p, 1, kAcceptTimeout);
            if (r <= 0) continue;
            if ((p.revents & POLLIN) == 0) continue;

            sockaddr_in peer_addr{};
            socklen_t   peer_len = sizeof(peer_addr);
            const int   fd = ::accept(
                listen_fd_, reinterpret_cast<sockaddr*>(&peer_addr),
                &peer_len);
            if (fd < 0) continue;

            // Inbound: create a partial link with empty peer_id,
            // run the reader; first frame must be Hello, which
            // fills peer_id and triggers on_connected_ +
            // registration.
            auto link = std::make_shared<PeerLink>();
            link->fd = fd;
            link->reader = std::thread(
                &TcpControlChannel::run_reader, this, link,
                /*we_initiated=*/false);
        }
    }

    /// @brief Reader-loop body. Drains frames until socket EOF.
    /// Owns its own link (shared_ptr keeps it alive past stop()).
    void run_reader(std::shared_ptr<PeerLink> link, bool we_initiated) {
        // If the remote opened to us, we owe them a Hello.
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
                // First frame must be Hello — that's how we learn
                // which peer this socket belongs to.
                if (h->type != MessageType::Hello) break;
                auto hello = decode_hello(payload.data(), payload.size());
                if (!hello || hello->machine_id.empty()) break;
                greeted = true;

                if (we_initiated) {
                    // Outbound link already registered with the
                    // expected peer_id; the inbound Hello just
                    // confirms protocol_version. We could swap the
                    // peer_id here if the remote name disagrees,
                    // but that's a future hardening pass.
                    if (on_connected_) on_connected_(link->peer_id);
                } else {
                    link->peer_id = hello->machine_id;
                    {
                        std::lock_guard lk(peers_m_);
                        // If we already have an outbound link to
                        // this peer, drop the inbound duplicate —
                        // first-Hello-wins keeps the topology
                        // consistent across both ends.
                        auto it = peers_.find(link->peer_id);
                        if (it != peers_.end() && it->second.get() != link.get()) {
                            return;  // reader thread exits; socket closed in dtor.
                        }
                        peers_[link->peer_id] = link;
                    }
                    int yes = 1;
                    ::setsockopt(link->fd, IPPROTO_TCP, TCP_NODELAY,
                                 &yes, sizeof(yes));
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

        // Drop this peer from the map (if it was the live entry)
        // and notify.
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
        if (link->fd != kInvalidFd) {
            ::shutdown(link->fd, SHUT_RDWR);
            ::close(link->fd);
            link->fd = kInvalidFd;
        }
        if (was_live && on_disconnected_) on_disconnected_(peer_id);
    }

    std::string                                            machine_id_;
    int                                                    listen_fd_   = kInvalidFd;
    std::uint16_t                                          listen_port_ = 0;
    std::atomic<bool>                                      accept_stop_{false};
    std::thread                                            accept_thread_;

    mutable std::mutex                                     peers_m_;
    std::unordered_map<std::string, std::shared_ptr<PeerLink>> peers_;

    OnFrameFn      on_frame_;
    OnPeerEventFn  on_connected_;
    OnPeerEventFn  on_disconnected_;
};

}  // namespace

std::unique_ptr<IControlChannel>
make_tcp_control_channel(const std::string& machine_id) {
    return std::make_unique<TcpControlChannel>(machine_id);
}

}  // namespace unio_ui::orchestrator::control
