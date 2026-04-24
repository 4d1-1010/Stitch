/// @file orchestrator.cpp
/// @brief Façade implementation: composes the seven internal
/// sub-modules, exposes the @ref IOrchestrator public surface,
/// and drives a worker thread that simulates the mesh timeline
/// (local probe → discovery → pairing → signed-in).

#include "orchestrator/orchestrator.hpp"

#include "orchestrator/crypto.hpp"

#include "mock_factories.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace unio_ui::orchestrator {

namespace {

/// @brief Fixed identity for the local peer in the mock.
constexpr const char* kLocalMachineId   = "adi-pc";
constexpr const char* kLocalDisplayName = "adi-pc (Linux)";

/// @brief Fixed identity for the simulated remote peer.
constexpr const char* kRemoteMachineId   = "diana";
constexpr const char* kRemoteDisplayName = "Diana (Windows)";

class FacadeOrchestrator final : public IOrchestrator {
public:
    explicit FacadeOrchestrator(OrchestratorCallbacks cb)
        : callbacks_(std::move(cb)),
          start_time_(std::chrono::steady_clock::now()),
          local_probe_(make_mock_local_probe()),
          mesh_(make_mock_mesh_crdt(kLocalMachineId)),
          discovery_(make_mock_discovery()),
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
    std::string local_machine_id()   const override { return kLocalMachineId; }
    std::string local_display_name() const override { return kLocalDisplayName; }

    AuthState auth_state() const override {
        const auto elapsed = std::chrono::steady_clock::now() - start_time_;
        if (elapsed < std::chrono::seconds(2)) return AuthState::GracePeriod;
        return AuthState::SignedIn;
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
    /// insert the local peer into our peer map.
    void publish_local_caps() {
        auto caps = local_probe_->probe();
        mesh_->put_caps(caps);
        mesh_->put_presence(PresenceRecord{});

        Peer local;
        local.machine_id   = kLocalMachineId;
        local.display_name = kLocalDisplayName;
        local.address      = "192.168.1.100";
        local.paired   = true;
        local.online   = true;
        local.is_local = true;

        std::lock_guard lk(peers_m_);
        peers_[kLocalMachineId] = local;
    }

    /// @brief Worker-thread body: drives the simulated timeline.
    void run() {
        wait_until(std::chrono::milliseconds(1000));
        if (stop_flag_.load()) return;

        discovery_->start(
            [this](const DiscoveryAnnouncement& a) { on_peer_observed(a); },
            [](const std::string& /*mid*/) {});

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

    /// @brief Handle a discovery announcement: upsert the peer,
    /// auto-pair, open the control connection, publish a mock
    /// remote caps record, fire @c on_peer_joined.
    void on_peer_observed(const DiscoveryAnnouncement& a) {
        const bool first_seen = [&] {
            std::lock_guard lk(peers_m_);
            return peers_.find(a.machine_id) == peers_.end();
        }();
        if (!first_seen) return;

        pairing_->accept(a.machine_id);
        control_->ensure_connection(a.machine_id, a.address, a.control_port);

        CapsRecord remote_caps;
        remote_caps.machine_id   = a.machine_id;
        remote_caps.display_name = kRemoteDisplayName;
        remote_caps.displays = {
            {a.machine_id, "\\\\.\\DISPLAY1", 4480, 0, 1920, 1080, 1},
            {a.machine_id, "\\\\.\\DISPLAY2", 6400, 0, 1920, 1080, 2},
        };
        remote_caps.encoders         = {"nvenc", "onevpl"};
        remote_caps.decoders         = {"d3d11va", "onevpl"};
        remote_caps.presenters       = {"dxgi-flip"};
        remote_caps.capture_backends = {"wgc"};
        mesh_->put_caps(remote_caps);

        Peer p;
        p.machine_id   = a.machine_id;
        p.display_name = a.display_name;
        p.address      = a.address;
        p.paired = true;
        p.online = true;
        {
            std::lock_guard lk(peers_m_);
            peers_[a.machine_id] = p;
        }
        if (callbacks_.on_peer_joined) callbacks_.on_peer_joined(p);
        if (callbacks_.on_peer_capabilities_changed) {
            callbacks_.on_peer_capabilities_changed(a.machine_id);
        }
    }

    OrchestratorCallbacks                            callbacks_;
    std::chrono::steady_clock::time_point            start_time_;

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
};

}  // namespace

std::unique_ptr<IOrchestrator>
make_mock(const OrchestratorCallbacks& callbacks) {
    return std::make_unique<FacadeOrchestrator>(callbacks);
}

}  // namespace unio_ui::orchestrator
