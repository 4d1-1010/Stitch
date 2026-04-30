/// @file lan_discovery.cpp
/// @brief Real LAN-discovery service.
///
/// Scope: orchestrate the three building blocks (UdpSocket,
/// enumerate_lan_interfaces, announce_codec) into a single
/// `IDiscovery` impl. One worker thread:
///   * sends an announce datagram on every enumerated interface
///     every @c kAnnounceInterval;
///   * drains inbound datagrams with a short poll timeout so the
///     thread can also tick announce + sweep without busy-waiting;
///   * sweeps peers whose last-seen exceeded @c kAnnounceTtl.
///
/// State is kept private to the worker; @c IDiscovery callbacks
/// are invoked from that thread (consistent with the mock impl
/// the orchestrator façade already handles).

#include "orchestrator/net/lan_discovery.hpp"

#include "orchestrator/net/announce_codec.hpp"
#include "orchestrator/net/lan_interfaces.hpp"
#include "orchestrator/net/udp_socket.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xorio::orchestrator::net {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kAnnounceInterval = std::chrono::seconds(2);
constexpr auto kAnnounceTtl      = std::chrono::seconds(10);

/// @brief Default transport port the announce advertises. Real
/// mesh wiring lives in the (still mock) control-connection
/// sub-module; until that opens a real listener, callers report
/// the port they intend to listen on. Keeping the constant local
/// rather than a magic number in the header.
constexpr std::uint16_t kDefaultMeshControlPort = 24800;

/// @brief Re-enumerate every N announce ticks so cable plug/unplug
/// + DHCP renewals are picked up without restarting the service.
constexpr int kReenumerateEveryTicks = 5;  // ≈ 10 s

/// @brief Per-peer state held by the worker thread.
struct TrackedPeer {
    AnnouncePayload   payload;
    std::string       source_ip;     ///< Last datagram source.
    Clock::time_point last_seen{};
    /// @brief Highest identify_request_id observed from this peer
    /// so far. `on_peer_identify` fires only when a higher value
    /// arrives — first-sighting bumps just initialise without
    /// triggering, so a peer that joins mid-mesh doesn't auto-fire.
    std::uint64_t     identify_seen = 0;
};

class LanDiscovery final : public IDiscovery {
public:
    explicit LanDiscovery(LanDiscoveryConfig cfg)
        : cfg_(std::move(cfg)) {
        if (cfg_.tcp_port == 0) cfg_.tcp_port = kDefaultMeshControlPort;
    }

    ~LanDiscovery() override { stop(); }

    void start(PeerObservedFn on_peer_observed,
               PeerLostFn     on_peer_lost) override {
        on_observed_ = std::move(on_peer_observed);
        on_lost_     = std::move(on_peer_lost);
        stop_flag_.store(false);
        worker_ = std::thread(&LanDiscovery::run, this);
    }

    void stop() override {
        stop_flag_.store(true);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void trigger_announce_now() override {
        // Two-stage delivery:
        //
        //   1. Send a one-shot broadcast SYNCHRONOUSLY on the
        //      calling thread so peers see the bump in
        //      milliseconds. The worker loop's recv-timeout
        //      (~200 ms) would otherwise gate how fast our own
        //      cadenced tick runs.
        //   2. Set the flag the worker reads on its next
        //      iteration so the next regularly-scheduled tick
        //      isn't a stale duplicate of what we just sent.
        send_announce_immediately();
        announce_now_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

    void accept_manual_invite(const std::string& /*invite_code*/) override {
        // Manual-invite path is the orchestrator's responsibility
        // — discovery here only delivers what the wire produced.
    }

private:
    /// @brief Entry-point of the worker thread.
    void run() {
        auto listener = open_listener();
        if (!listener) return;

        auto broadcasters = open_broadcasters();
        Clock::time_point last_enumerate = Clock::now();

        Clock::time_point next_announce = Clock::now();
        int               tick_count    = 0;

        while (!stop_flag_.load()) {
            // 1) Drain inbound for up to ~200 ms so the loop also
            //    has a chance to tick announce + sweep.
            constexpr auto kPollSlice = std::chrono::milliseconds(200);
            const auto before = Clock::now();
            if (auto dg = listener->recv_with_timeout(kPollSlice); dg) {
                handle_datagram(*dg);
            }

            // 2) Re-enumerate periodically so plugs/unplugs surface
            //    without restarting the service.
            if (Clock::now() - last_enumerate
                >= kAnnounceInterval * kReenumerateEveryTicks) {
                broadcasters    = open_broadcasters();
                last_enumerate = Clock::now();
            }

            // 3) Tick announce — fires on the regular cadence or
            //    immediately when an external caller (the
            //    orchestrator's request_identify path) bumped the
            //    flag via trigger_announce_now().
            const bool forced = announce_now_.exchange(false,
                                          std::memory_order_acq_rel);
            if (forced || Clock::now() >= next_announce) {
                tick_announce(broadcasters);
                next_announce = Clock::now() + kAnnounceInterval;
                ++tick_count;
            }

            // 4) Sweep stale peers on every iteration — cheap.
            sweep_stale();

            // Safety net: if the recv timed out instantly (e.g. a
            // broken poll), don't busy-spin.
            if (Clock::now() - before < std::chrono::milliseconds(1)) {
                std::unique_lock lk(cv_m_);
                cv_.wait_for(lk, std::chrono::milliseconds(50));
            }
        }
    }

    /// @brief Open the inbound listener bound to 0.0.0.0:24801.
    std::unique_ptr<UdpSocket> open_listener() {
        auto s = UdpSocket::open();
        if (!s) return nullptr;
        if (!s->set_reuse_addr(true))               return nullptr;
        if (!s->set_broadcast(true))                return nullptr;
        if (!s->bind("0.0.0.0", kAnnouncePort))     return nullptr;
        return s;
    }

    /// @brief Open one broadcast socket per enumerated NIC, bound
    /// to the NIC's own IP. Binding to the interface IP rather
    /// than 0.0.0.0 forces the kernel to send on that NIC, which
    /// matters on multi-homed hosts (cable + WiFi) where a 0.0.0.0
    /// broadcast follows the default route only.
    std::vector<std::pair<std::unique_ptr<UdpSocket>, std::string>>
    open_broadcasters() {
        std::vector<std::pair<std::unique_ptr<UdpSocket>, std::string>> out;
        for (const auto& iface : enumerate_lan_interfaces()) {
            auto s = UdpSocket::open();
            if (!s) continue;
            if (!s->set_reuse_addr(true))   continue;
            if (!s->set_broadcast(true))    continue;
            // Bind to (iface_ip, 0) — kernel picks an ephemeral
            // source port. Discovery only sends + listens on the
            // listener; broadcasters are write-only.
            if (!s->bind(iface.ip, 0))      continue;
            const std::string dest = iface.broadcast_ip.empty()
                                     ? std::string("255.255.255.255")
                                     : iface.broadcast_ip;
            out.emplace_back(std::move(s), dest);
        }
        return out;
    }

    /// @brief Snapshot the AnnouncePayload from the live cfg_
    /// state. Safe to call from any thread — every input
    /// (atomic flag, callback, string fields set at construction)
    /// is itself thread-safe.
    AnnouncePayload build_payload() const {
        AnnouncePayload p;
        p.machine_id = cfg_.machine_id;
        p.hostname   = cfg_.hostname;
        p.tcp_port   = cfg_.tcp_port;
        p.data_port  = cfg_.data_port;
        p.authed = cfg_.authed_flag != nullptr
                 && cfg_.authed_flag->load(std::memory_order_acquire);
        if (cfg_.displays_provider) {
            p.displays = cfg_.displays_provider();
        }
        if (cfg_.workspaces_provider) {
            p.workspaces = cfg_.workspaces_provider();
        }
        if (cfg_.identify_counter != nullptr) {
            p.identify_request_id =
                cfg_.identify_counter->load(std::memory_order_acquire);
        }
        return p;
    }

    /// @brief Build + send an announce datagram on every broadcaster.
    void tick_announce(
        const std::vector<std::pair<std::unique_ptr<UdpSocket>,
                                    std::string>>& broadcasters) {
        const auto bytes = encode_announce(build_payload());
        for (const auto& [sock, dest] : broadcasters) {
            sock->send_to(bytes.data(), bytes.size(),
                          dest, kAnnouncePort);
        }
    }

    /// @brief One-shot broadcast on every enumerated interface,
    /// run on the caller's thread. Used by `trigger_announce_now`
    /// to make state-change announces (Identify, sign-in, …)
    /// reach peers without waiting for the worker's next tick.
    void send_announce_immediately() {
        const auto bytes = encode_announce(build_payload());
        auto sock = UdpSocket::open();
        if (!sock) return;
        sock->set_broadcast(true);
        for (const auto& iface : enumerate_lan_interfaces()) {
            const std::string dest = iface.broadcast_ip.empty()
                                      ? std::string("255.255.255.255")
                                      : iface.broadcast_ip;
            sock->send_to(bytes.data(), bytes.size(),
                          dest, kAnnouncePort);
        }
    }

    /// @brief Process one inbound datagram.
    void handle_datagram(const ReceivedDatagram& dg) {
        auto parsed = decode_announce(dg.bytes.data(), dg.bytes.size());
        if (!parsed) return;
        if (parsed->machine_id == cfg_.machine_id) return;  // self-echo.

        const auto now = Clock::now();
        bool first_seen           = false;
        bool identify_bumped      = false;
        {
            std::lock_guard lk(peers_m_);
            auto& tp = peers_[parsed->machine_id];
            first_seen = tp.last_seen.time_since_epoch().count() == 0;

            // Identify-bump detection. First sighting: latch the
            // current value without firing. Subsequent: fire only
            // on strictly-greater values (counter monotone, so
            // wraparound isn't a concern at u64 precision).
            if (first_seen) {
                tp.identify_seen = parsed->identify_request_id;
            } else if (parsed->identify_request_id > tp.identify_seen) {
                tp.identify_seen = parsed->identify_request_id;
                identify_bumped  = true;
            }

            tp.payload   = *parsed;
            tp.source_ip = dg.source_ip;
            tp.last_seen = now;
        }
        if (identify_bumped && cfg_.on_peer_identify) {
            cfg_.on_peer_identify(parsed->machine_id);
        }

        DiscoveryAnnouncement a;
        a.machine_id   = parsed->machine_id;
        a.display_name = parsed->hostname;
        a.address      = dg.source_ip;
        a.control_port = parsed->tcp_port;
        a.data_port    = parsed->data_port;
        a.authed       = parsed->authed;
        a.displays.reserve(parsed->displays.size());
        for (const auto& wire : parsed->displays) {
            Display d;
            d.machine_id = parsed->machine_id;
            d.monitor_id = wire.monitor_id;
            d.global_x   = wire.global_x;
            d.global_y   = wire.global_y;
            d.width      = wire.width;
            d.height     = wire.height;
            d.number     = static_cast<std::int32_t>(a.displays.size() + 1);
            a.displays.push_back(std::move(d));
        }
        a.workspaces.reserve(parsed->workspaces.size());
        for (const auto& wire : parsed->workspaces) {
            Workspace ws;
            ws.id         = wire.id;
            ws.name       = wire.name;
            ws.version_ns = wire.version_ns;
            ws.tombstone  = wire.tombstone;
            ws.input_members.reserve(wire.input_members.size());
            for (const auto& m : wire.input_members) ws.input_members.insert(m);
            ws.clipboard_members.reserve(wire.clipboard_members.size());
            for (const auto& m : wire.clipboard_members) ws.clipboard_members.insert(m);
            // Re-derive `members` from input ∪ clipboard. If the
            // sender omitted both per-cap lists (older build that
            // only had `members`), fall back to the wire's union
            // list and promote it to every per-cap set so we don't
            // silently strip features on either side. The codec's
            // 3-list legacy probe already folded any historical
            // keyboard_members into input_members on parse, so
            // there's no separate keyboard set to merge here.
            if (ws.input_members.empty()
                && ws.clipboard_members.empty()
                && !wire.members.empty()) {
                for (const auto& m : wire.members) {
                    ws.input_members.insert(m);
                    ws.clipboard_members.insert(m);
                }
            }
            ws.members = ws.input_members;
            for (const auto& m : ws.clipboard_members) ws.members.insert(m);
            const std::uint8_t cm = wire.clipboard_max;
            if (cm <= static_cast<std::uint8_t>(ClipboardLimit::Unlimited)) {
                ws.clipboard_max = static_cast<ClipboardLimit>(cm);
            }
            ws.clipboard_rich          = wire.clipboard_rich;
            ws.clipboard_files         = wire.clipboard_files;
            ws.cursor_edge_margin      = wire.cursor_edge_margin;
            ws.cursor_require_modifier = wire.cursor_require_modifier;
            ws.cursor_block_hotkeys    = wire.cursor_block_hotkeys;
            const std::uint8_t au = wire.auto_unlock;
            if (au <= static_cast<std::uint8_t>(AutoUnlock::Hour1)) {
                ws.auto_unlock = static_cast<AutoUnlock>(au);
            }
            ws.layout.reserve(wire.layout.size());
            for (const auto& we : wire.layout) {
                DisplayLayoutEntry e;
                e.machine_id = we.machine_id;
                e.monitor_id = we.monitor_id;
                e.global_x   = we.global_x;
                e.global_y   = we.global_y;
                ws.layout.push_back(std::move(e));
            }
            for (const auto& s : wire.member_stamps) {
                ws.member_stamps[s.machine_id] =
                    MemberStamp{s.is_member, s.logical_clock};
            }
            ws.locked                     = wire.locked;
            ws.lock_unlock_after_h        = wire.lock_unlock_after_h;
            ws.master_locked              = wire.master_locked;
            ws.master_lock_unlock_after_h = wire.master_lock_unlock_after_h;
            ws.master_locked_by           = wire.master_locked_by;
            a.workspaces.push_back(std::move(ws));
        }
        a.version_ns   = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                now.time_since_epoch()).count());

        // Refresh callbacks fire on every announcement (matches the
        // IDiscovery contract — re-emits are how listeners refresh
        // their own staleness clocks). Reserved `first_seen` for a
        // future "added" vs "refreshed" split if it becomes useful.
        (void)first_seen;
        if (on_observed_) on_observed_(a);
    }

    /// @brief Evict peers that haven't announced inside the TTL.
    void sweep_stale() {
        const auto now = Clock::now();
        std::vector<std::string> evicted;
        {
            std::lock_guard lk(peers_m_);
            for (auto it = peers_.begin(); it != peers_.end();) {
                if (now - it->second.last_seen > kAnnounceTtl) {
                    evicted.push_back(it->first);
                    it = peers_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (on_lost_) {
            for (const auto& mid : evicted) on_lost_(mid);
        }
    }

    LanDiscoveryConfig                          cfg_;
    PeerObservedFn                              on_observed_;
    PeerLostFn                                  on_lost_;

    std::thread                                 worker_;
    std::atomic<bool>                           stop_flag_{false};
    std::mutex                                  cv_m_;
    std::condition_variable                     cv_;

    mutable std::mutex                          peers_m_;
    std::unordered_map<std::string, TrackedPeer> peers_;

    /// @brief Set by `trigger_announce_now()` to make the next
    /// run-loop iteration tick announce regardless of the cadence.
    std::atomic<bool>                           announce_now_{false};
};

}  // namespace

std::unique_ptr<IDiscovery>
make_lan_discovery(const LanDiscoveryConfig& config) {
    return std::make_unique<LanDiscovery>(config);
}

}  // namespace xorio::orchestrator::net
