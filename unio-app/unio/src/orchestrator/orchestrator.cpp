/// @file orchestrator.cpp
/// @brief Façade implementation: composes the seven internal
/// sub-modules, exposes the @ref IOrchestrator public surface,
/// and drives a worker thread that simulates the mesh timeline
/// (local probe → discovery → pairing → signed-in).

#include "orchestrator/orchestrator.hpp"

#include "orchestrator/crypto.hpp"
#include "orchestrator/local_probe.hpp"
#include "orchestrator/net/lan_discovery.hpp"

#include "mock/factories.hpp"

#include <algorithm>
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
/// crypto branch lands. Dev-only — not a secret in the security
/// sense.
constexpr const char* kAccessGateKey = "admin";

class FacadeOrchestrator final : public IOrchestrator {
public:
    explicit FacadeOrchestrator(OrchestratorCallbacks cb)
        : callbacks_(std::move(cb)),
          start_time_(std::chrono::steady_clock::now()),
          local_machine_id_(local_hostname()),
          local_display_name_(local_machine_id_),
          local_probe_(make_local_probe()),
          mesh_(make_mock_mesh_crdt(local_machine_id_)),
          discovery_(net::make_lan_discovery(net::LanDiscoveryConfig{
              local_machine_id_,
              local_display_name_,
              /*tcp_port=*/0,         // Defaulted by LanDiscovery for now.
              &access_authorized_,    // Live source of "user signed in".
              [this]() {              // Live source of local displays.
                  return wire_displays_for_announce();
              },
              &identify_counter_,     // Live identify counter.
              [this](const std::string& /*peer_mid*/) {
                  // Remote peer clicked Identify — fire our local
                  // overlay through the same callback the local
                  // click uses.
                  if (callbacks_.on_identify_request) {
                      callbacks_.on_identify_request();
                  }
              }
          })),
          pairing_(make_mock_pairing_manager()),
          control_(make_mock_control_connection_manager()),
          media_(make_mock_media_connection_factory()),
          selector_(make_mock_path_selector()),
          scheduler_(make_mock_session_scheduler()),
          workspaces_(make_mock_workspace_manager()) {
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

    /// @brief Aggregate every peer's displays into a single list
    /// with **mesh-wide unique numbers**. Each display's `number`
    /// field is reassigned 1..N in (machine_id, monitor_id)
    /// alphabetical order so two peers' "primary" monitor don't
    /// both get number 1; the Identify overlay + UI both rely on
    /// this being unique. Same ordering rule the Python tree's
    /// `_number_all_monitors` uses.
    std::vector<Display> displays() const override {
        std::vector<Display> out;
        for (const auto& [_, caps] : mesh_->all_caps()) {
            for (const auto& d : caps.displays) out.push_back(d);
        }
        std::sort(out.begin(), out.end(),
                  [](const Display& a, const Display& b) {
                      if (a.machine_id != b.machine_id) {
                          return a.machine_id < b.machine_id;
                      }
                      return a.monitor_id < b.monitor_id;
                  });
        std::int32_t n = 1;
        for (auto& d : out) d.number = n++;
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

    void request_identify() override {
        identify_counter_.fetch_add(1, std::memory_order_acq_rel);
        if (callbacks_.on_identify_request) {
            callbacks_.on_identify_request();
        }
    }

    // ── Workspace queries ──────────────────────────────────
    std::vector<Workspace> workspaces() const override {
        return workspaces_->list();
    }

    std::optional<Workspace>
    workspace(const std::string& id) const override {
        return workspaces_->get(id);
    }

    std::vector<std::pair<std::string, std::string>>
    pc_workspace_assignments() const override {
        return workspaces_->pc_assignments();
    }

    // ── Workspace actions ──────────────────────────────────
    std::string
    create_workspace(const std::string& name,
                     const std::unordered_set<std::string>& members) override {
        return workspaces_->create(name, members);
    }

    void rename_workspace(const std::string& workspace_id,
                          const std::string& new_name) override {
        workspaces_->rename(workspace_id, new_name);
    }

    void set_workspace_members(
        const std::string& workspace_id,
        const std::unordered_set<std::string>& members) override {
        workspaces_->set_members(workspace_id, members);
    }

    void delete_workspace(const std::string& workspace_id) override {
        workspaces_->destroy(workspace_id);
    }

    void acquire_workspace_lock(const std::string& workspace_id) override {
        workspaces_->acquire_lock(workspace_id, local_machine_id_);
    }

    void release_workspace_lock(const std::string& workspace_id) override {
        workspaces_->release_lock(workspace_id, local_machine_id_);
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
    /// insert the local peer into our peer map. The probe's mock
    /// impl uses a hardcoded machine_id ("adi-pc"); we overwrite
    /// it (plus every embedded display reference) with the real
    /// local hostname so the Layout tab's identity routes hook up
    /// correctly. Real platform-aware probes will return the
    /// right machine_id natively and this rewrite becomes a no-op.
    void publish_local_caps() {
        auto caps = local_probe_->probe();
        caps.machine_id   = local_machine_id_;
        caps.display_name = local_display_name_;
        for (auto& d : caps.displays) d.machine_id = local_machine_id_;
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

    /// @brief Snapshot the local probe's displays for the next
    /// announce datagram. Called from the LanDiscovery worker
    /// thread once per tick.
    std::vector<net::AnnounceDisplay> wire_displays_for_announce() const {
        std::vector<net::AnnounceDisplay> out;
        const auto caps = local_probe_->probe();
        out.reserve(caps.displays.size());
        for (const auto& d : caps.displays) {
            net::AnnounceDisplay w;
            w.monitor_id = d.monitor_id;
            w.global_x   = d.global_x;
            w.global_y   = d.global_y;
            w.width      = d.width;
            w.height     = d.height;
            out.push_back(std::move(w));
        }
        return out;
    }

    /// @brief Synthesise a fallback caps record for a peer whose
    /// announce arrived without the displays extension (legacy
    /// Python instances or pre-R2 builds). Real-display peers
    /// supply their own geometry on the wire — see
    /// @ref on_peer_observed.
    static CapsRecord synthesize_peer_caps(const std::string& machine_id,
                                           const std::string& display_name) {
        // Stable hash → small X offset relative to the local
        // probe's 0..4480 window. Range 0..3 buckets × 600 px =
        // 0..1800 of jitter; the base offset (5000) plus the
        // largest jitter still keeps every peer inside the
        // ~7000 px-wide global desktop strip.
        std::uint64_t h = 1469598103934665603ull;          // FNV-1a init
        for (unsigned char c : machine_id) {
            h ^= c;
            h *= 1099511628211ull;
        }
        const std::int32_t base_x =
            5000 + static_cast<std::int32_t>((h % 4u) * 600u);

        CapsRecord c;
        c.machine_id   = machine_id;
        c.display_name = display_name.empty() ? machine_id : display_name;
        c.displays.push_back(Display{
            machine_id, "DISPLAY1",
            base_x,        0, 1920, 1080, 1
        });
        c.displays.push_back(Display{
            machine_id, "DISPLAY2",
            base_x + 1920, 0, 2560, 1440, 2
        });
        return c;
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
    ///
    /// Also handles auto-activation: when any mesh peer reports
    /// `authed:true` and the local user hasn't yet typed the
    /// access-gate key, we flip our own authorized flag so every
    /// PC in the mesh unlocks once any one of them signs in.
    void on_peer_observed(const DiscoveryAnnouncement& a) {
        if (a.authed && !access_authorized_.load(std::memory_order_acquire)) {
            access_authorized_.store(true, std::memory_order_release);
        }

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

        // Refresh the peer's caps every announce — peers' real
        // displays come over the wire now, and we want plug/unplug
        // events to surface in the mesh on the next 2 s tick.
        // Mock CRDT writes are idempotent; the cost is minimal.
        if (!a.displays.empty()) {
            CapsRecord c;
            c.machine_id   = a.machine_id;
            c.display_name = a.display_name.empty() ? a.machine_id
                                                     : a.display_name;
            c.displays     = a.displays;
            mesh_->put_caps(std::move(c));
        } else {
            // Wire payload didn't carry displays (older Python
            // instance or pre-R2 build) — placeholder so the
            // Layout still has something to show for this peer.
            mesh_->put_caps(synthesize_peer_caps(a.machine_id,
                                                  a.display_name));
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
            if (callbacks_.on_peer_capabilities_changed) {
                callbacks_.on_peer_capabilities_changed(a.machine_id);
            }
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
    std::unique_ptr<IWorkspaceManager>               workspaces_;

    mutable std::mutex                               peers_m_;
    std::unordered_map<std::string, Peer>            peers_;

    std::thread                                      worker_;
    std::atomic<bool>                                stop_flag_{false};
    std::mutex                                       cv_m_;
    std::condition_variable                          cv_;

    std::atomic<bool>                                access_authorized_{false};
    std::atomic<std::uint64_t>                       identify_counter_{0};
};

}  // namespace

std::unique_ptr<IOrchestrator>
make_mock(const OrchestratorCallbacks& callbacks) {
    return std::make_unique<FacadeOrchestrator>(callbacks);
}

}  // namespace unio_ui::orchestrator
