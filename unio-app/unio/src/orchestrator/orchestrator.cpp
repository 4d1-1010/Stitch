/// @file orchestrator.cpp
/// @brief Façade implementation: composes the seven internal
/// sub-modules, exposes the @ref IOrchestrator public surface,
/// and drives a worker thread that simulates the mesh timeline
/// (local probe → discovery → pairing → signed-in).

#include "orchestrator/orchestrator.hpp"

#include "orchestrator/crypto.hpp"
#include "orchestrator/net/lan_discovery.hpp"

#include "mock/factories.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <unistd.h>
#endif

namespace unio_ui::orchestrator {

namespace {

/// @brief Read the OS-reported host name. Falls back to a constant
/// so the rest of the façade always has a non-empty string. Used
/// as both the announce hostname and the local machine_id until a
/// stable hardware-bound identifier lands.
std::string local_hostname() {
#if defined(_WIN32)
    char buf[256] = {};
    DWORD len = sizeof(buf);
    if (::GetComputerNameA(buf, &len)) return std::string(buf, len);
#else
    char buf[256] = {};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) return buf;
#endif
    return "unknown-host";
}

/// @brief Hardcoded access-gate key. Pre-launch placeholder; the
/// real licence-token verifier replaces this when commit B's
/// crypto branch lands. Picked to be inert (no real semantic
/// meaning) and dev-only; not a secret in the security sense.
constexpr const char* kAccessGateKey = "unio-app-2026";

class FacadeOrchestrator final : public IOrchestrator {
public:
    explicit FacadeOrchestrator(OrchestratorCallbacks cb)
        : callbacks_(std::move(cb)),
          start_time_(std::chrono::steady_clock::now()),
          local_machine_id_(local_hostname()),
          local_display_name_(local_machine_id_),
          local_probe_(make_mock_local_probe()),
          mesh_(make_mock_mesh_crdt(local_machine_id_)),
          discovery_(net::make_lan_discovery(net::LanDiscoveryConfig{
              local_machine_id_,
              local_display_name_,
              /*tcp_port=*/0  // Defaulted by LanDiscovery for now.
          })),
          pairing_(make_mock_pairing_manager()),
          control_(make_mock_control_connection_manager()),
          media_(make_mock_media_connection_factory()),
          selector_(make_mock_path_selector()),
          scheduler_(make_mock_session_scheduler()) {
        wire_sub_modules();
        publish_local_caps();
        worker_ = std::thread(&FacadeOrchestrator::run, this);
    }

    ~FacadeOrchestrator() override {
        stop_flag_.store(true);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        discovery_->stop();
    }

    // ── Identity queries ───────────────────────────────────
    std::string local_machine_id()   const override { return local_machine_id_; }
    std::string local_display_name() const override { return local_display_name_; }

    AuthState auth_state() const override {
        const auto elapsed = std::chrono::steady_clock::now() - start_time_;
        if (elapsed < std::chrono::seconds(2)) return AuthState::GracePeriod;
        return AuthState::SignedIn;
    }

    bool access_authorized() const override {
        return access_authorized_.load(std::memory_order_acquire);
    }

    bool try_authorize(const std::string& key) override {
        if (key != kAccessGateKey) return false;
        access_authorized_.store(true, std::memory_order_release);
        return true;
    }

    // ── Mesh queries ───────────────────────────────────────
    std::vector<Peer> peers() const override {
        std::lock_guard lk(peers_m_);
        std::vector<Peer> out;
        out.reserve(peers_.size());
        for (const auto& [_, p] : peers_) out.push_back(p);
        return out;
    }

    std::vector<Display> displays() const override {
        std::vector<Display> out;
        for (const auto& [_, caps] : mesh_->all_caps()) {
            for (const auto& d : caps.displays) out.push_back(d);
        }
        return out;
    }

    StreamState stream_state(StreamId id) const override {
        return scheduler_->state_of(id);
    }

    // ── Actions ────────────────────────────────────────────
    StreamId start_stream(DisplayRef src, DisplayRef dst,
                          RoutingMode mode) override {
        auto [id, outcome] = scheduler_->start(std::move(src),
                                               std::move(dst), mode);
        if (outcome == StartOutcome::Accepted && callbacks_.on_stream_started) {
            callbacks_.on_stream_started(id);
        }
        return id;
    }

    void stop_stream(StreamId id) override {
        scheduler_->stop(id);
        if (callbacks_.on_stream_stopped) callbacks_.on_stream_stopped(id);
    }

    void request_pair(const std::string& machine_id,
                      const std::string& invite_code) override {
        pairing_->request(machine_id, invite_code);
    }

    void accept_pairing(const std::string& machine_id) override {
        pairing_->accept(machine_id);
    }

    void reject_pairing(const std::string& machine_id,
                        const std::string& reason) override {
        pairing_->reject(machine_id, reason);
    }

    void unpair(const std::string& machine_id) override {
        pairing_->unpair(machine_id);
        control_->close_connection(machine_id);
    }

private:
    /// @brief Install cross-module callbacks.
    void wire_sub_modules() {
        pairing_->set_callbacks(
            [this](const std::string& mid, const std::string& code) {
                if (callbacks_.on_pairing_request) {
                    callbacks_.on_pairing_request(mid, code);
                }
            },
            [this](const std::string& mid,
                   PairingOutcome outcome,
                   const std::string& reason) {
                if (outcome == PairingOutcome::Accepted) {
                    if (callbacks_.on_pairing_accepted) {
                        callbacks_.on_pairing_accepted(mid);
                    }
                } else {
                    if (callbacks_.on_pairing_rejected) {
                        callbacks_.on_pairing_rejected(mid, reason);
                    }
                }
            });

        scheduler_->set_callbacks(
            [this](StreamId id, StreamState state) {
                if (state == StreamState::Running &&
                    callbacks_.on_stream_recovered) {
                    callbacks_.on_stream_recovered(id);
                }
            },
            [this](StreamId id, const std::string& reason) {
                if (callbacks_.on_stream_failed) {
                    callbacks_.on_stream_failed(id, reason);
                }
            });
    }

    /// @brief Probe local capabilities, publish to the mesh, and
    /// insert the local peer into our peer map. The address field
    /// stays empty here — it's filled in later if the discovery
    /// subsystem ever needs to surface "this is me" reachability.
    void publish_local_caps() {
        auto caps = local_probe_->probe();
        mesh_->put_caps(caps);
        mesh_->put_presence(PresenceRecord{});

        Peer local;
        local.machine_id   = local_machine_id_;
        local.display_name = local_display_name_;
        local.address      = {};
        local.paired   = true;
        local.online   = true;
        local.is_local = true;

        std::lock_guard lk(peers_m_);
        peers_[local_machine_id_] = local;
    }

    /// @brief Worker-thread body: kicks off real LAN discovery and
    /// fires the auth-state-changed transition once. Real discovery
    /// runs continuously inside its own thread; this loop's only
    /// remaining job is the post-grace-period notification, kept
    /// because the UI's Activity tab queries `auth_state()` on it.
    void run() {
        wait_until(std::chrono::milliseconds(1000));
        if (stop_flag_.load()) return;

        discovery_->start(
            [this](const DiscoveryAnnouncement& a) { on_peer_observed(a); },
            [this](const std::string& mid)        { on_peer_lost(mid); });

        wait_until(std::chrono::milliseconds(2000));
        if (stop_flag_.load()) return;
        if (callbacks_.on_auth_state_changed) {
            callbacks_.on_auth_state_changed();
        }
    }

    /// @brief Sleep until @p delay has elapsed from construction
    /// or @ref stop_flag_ is set, whichever comes first.
    void wait_until(std::chrono::milliseconds delay) {
        std::unique_lock lk(cv_m_);
        cv_.wait_until(lk, start_time_ + delay,
                       [&] { return stop_flag_.load(); });
    }

    /// @brief Handle a discovery announcement.
    ///
    /// Real discovery refreshes the peer entry on every datagram;
    /// the first sighting fires `on_peer_joined`, subsequent
    /// refreshes only update the cached row. Capabilities are
    /// not published here — that's a separate sub-module's job
    /// once a control channel is open and the peer reports its
    /// real hardware. For the Activity tab today the peer card
    /// only needs machine_id + display_name + address.
    void on_peer_observed(const DiscoveryAnnouncement& a) {
        bool first_seen = false;
        {
            std::lock_guard lk(peers_m_);
            auto it = peers_.find(a.machine_id);
            first_seen = (it == peers_.end());

            Peer p;
            p.machine_id   = a.machine_id;
            p.display_name = a.display_name.empty()
                             ? a.machine_id
                             : a.display_name;
            p.address      = a.address;
            p.paired = true;
            p.online = true;
            peers_[a.machine_id] = p;
        }

        if (first_seen) {
            // Auto-pair on first sighting matches the mock's
            // behaviour. Real pairing flow (PIN exchange, mutual
            // confirm) is the pairing sub-module's concern; this
            // call is a no-op on the mock impl.
            pairing_->accept(a.machine_id);
            control_->ensure_connection(a.machine_id,
                                        a.address, a.control_port);

            Peer p;
            {
                std::lock_guard lk(peers_m_);
                p = peers_[a.machine_id];
            }
            if (callbacks_.on_peer_joined) callbacks_.on_peer_joined(p);
        }
    }

    /// @brief Mirror of @ref on_peer_observed — drop the peer +
    /// notify the UI when discovery declares it stale (TTL elapsed).
    void on_peer_lost(const std::string& machine_id) {
        bool was_present = false;
        {
            std::lock_guard lk(peers_m_);
            was_present = peers_.erase(machine_id) > 0;
        }
        if (was_present && callbacks_.on_peer_left) {
            callbacks_.on_peer_left(machine_id);
        }
    }

    OrchestratorCallbacks                            callbacks_;
    std::chrono::steady_clock::time_point            start_time_;

    std::string                                      local_machine_id_;
    std::string                                      local_display_name_;

    std::unique_ptr<ILocalProbeAdapter>              local_probe_;
    std::unique_ptr<IMeshCrdt>                       mesh_;
    std::unique_ptr<IDiscovery>                      discovery_;
    std::unique_ptr<IPairingManager>                 pairing_;
    std::unique_ptr<IControlConnectionManager>       control_;
    std::unique_ptr<IMediaConnectionFactory>         media_;
    std::unique_ptr<IPathSelector>                   selector_;
    std::unique_ptr<ISessionScheduler>               scheduler_;

    mutable std::mutex                               peers_m_;
    std::unordered_map<std::string, Peer>            peers_;

    std::thread                                      worker_;
    std::atomic<bool>                                stop_flag_{false};
    std::mutex                                       cv_m_;
    std::condition_variable                          cv_;

    std::atomic<bool>                                access_authorized_{false};
};

}  // namespace

std::unique_ptr<IOrchestrator>
make_mock(const OrchestratorCallbacks& callbacks) {
    return std::make_unique<FacadeOrchestrator>(callbacks);
}

}  // namespace unio_ui::orchestrator
