/// @file orchestrator.cpp
/// @brief Façade implementation: composes the seven internal
/// sub-modules, exposes the @ref IOrchestrator public surface,
/// and drives a worker thread that simulates the mesh timeline
/// (local probe → discovery → pairing → signed-in).

#include "orchestrator/orchestrator.hpp"

#include "orchestrator/control/control_channel.hpp"
#include "orchestrator/control/protocol.hpp"
#include "orchestrator/crypto.hpp"
#include "orchestrator/cursor_router.hpp"
#include "orchestrator/input/cursor_poller.hpp"
#include "orchestrator/input/input_backend.hpp"
#include "orchestrator/local_probe.hpp"
#include "orchestrator/net/lan_discovery.hpp"
#include "orchestrator/peer_events.hpp"

#include "mock/factories.hpp"

#include <cstdio>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

/// @brief Build + start the TCP control channel; write the actual
/// bound port back to @p port_out so the LAN announce can advertise
/// it. Used from the constructor's init list — the side effect on
/// @p port_out happens *before* the discovery config is built,
/// since member-init order = declaration order.
std::unique_ptr<control::IControlChannel>
start_control_channel(const std::string& machine_id,
                       std::uint16_t&     port_out) {
    auto ch = control::make_tcp_control_channel(machine_id);
    if (!ch->start(0)) {
        std::fprintf(stderr, "control: channel start failed\n");
        port_out = 0;
        return ch;
    }
    port_out = ch->listen_port();
    std::fprintf(stderr,
                 "control: listening on port %u\n",
                 static_cast<unsigned>(port_out));
    return ch;
}

class FacadeOrchestrator final : public IOrchestrator {
public:
    explicit FacadeOrchestrator(OrchestratorCallbacks cb)
        : callbacks_(std::move(cb)),
          start_time_(std::chrono::steady_clock::now()),
          local_machine_id_(local_hostname()),
          local_display_name_(local_machine_id_),
          local_probe_(make_local_probe()),
          mesh_(make_mock_mesh_crdt(local_machine_id_)),
          // Workspaces sub-module is constructed BEFORE discovery
          // so the discovery config's workspaces_provider can
          // capture &*workspaces_ safely. Member-init order matches
          // declaration order in C++; declaration in this class
          // already places workspaces_ above discovery_-adjacent
          // members in the init list ordering below — but the C++
          // language requires member-init order = declaration
          // order, so we declare workspaces_ above discovery_ in
          // the data-member section further down.
          workspaces_(make_mock_workspace_manager()),
          // Real peer-to-peer TCP control channel. The helper
          // starts it and writes the bound port to control_port_
          // BEFORE the discovery config below reads control_port_
          // (init order matches declaration order: control_port_,
          // then control_channel_, then discovery_).
          control_channel_(start_control_channel(local_machine_id_,
                                                  control_port_)),
          input_backend_(input::make_default_input_backend()),
          discovery_(net::make_lan_discovery(net::LanDiscoveryConfig{
              local_machine_id_,
              local_display_name_,
              control_port_,          // Real bound port from above.
              &access_authorized_,    // Live source of "user signed in".
              [this]() {              // Live source of local displays.
                  return wire_displays_for_announce();
              },
              [this]() {              // Live source of local workspaces.
                  return wire_workspaces_for_announce();
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
          peer_events_(*mesh_, *pairing_, *control_, *workspaces_,
                       control_channel_.get(),
                       callbacks_, access_authorized_,
                       peers_m_, peers_) {
        wire_sub_modules();
        publish_local_caps();
        if (input_backend_) input_backend_->open();
        wire_control_channel_callbacks();
        wire_cursor_poller();
        wire_raw_input_capture();
        // Every workspace mutation fires an immediate announce so
        // peers see the change sub-second instead of waiting for
        // the next 2s tick. Both local mutations + remote merges
        // funnel through the on_changed callback; the broadcast
        // is idempotent (LWW on the receive side ignores no-op
        // updates), so re-broadcasting after a merge is harmless.
        workspaces_->set_on_changed(
            [this](const std::string& /*workspace_id*/) {
                if (discovery_) discovery_->trigger_announce_now();
                refresh_cursor_router_state();
            });
        refresh_cursor_router_state();
        worker_ = std::thread(&FacadeOrchestrator::run, this);
    }

    ~FacadeOrchestrator() override {
        stop_flag_.store(true);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (cursor_poller_)   cursor_poller_->stop();
        discovery_->stop();
        if (control_channel_) control_channel_->stop();
        if (input_backend_)   input_backend_->close();
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
        // Force an immediate announce so peers see the bump
        // without the up-to-2-second wait of the regular cadence.
        if (discovery_) discovery_->trigger_announce_now();
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
                     const std::unordered_set<std::string>& members,
                     const std::unordered_set<std::string>& input_members,
                     const std::unordered_set<std::string>& clipboard_members) override {
        return workspaces_->create(name, members, input_members,
                                    clipboard_members);
    }

    void rename_workspace(const std::string& workspace_id,
                          const std::string& new_name) override {
        workspaces_->rename(workspace_id, new_name);
    }

    void set_workspace_members(
        const std::string& workspace_id,
        const std::unordered_set<std::string>& members,
        const std::unordered_set<std::string>& input_members,
        const std::unordered_set<std::string>& clipboard_members) override {
        workspaces_->set_members(workspace_id, members, input_members,
                                  clipboard_members);
    }

    void set_workspace_settings(
        const std::string& workspace_id,
        const WorkspaceSettings& settings) override {
        workspaces_->set_settings(workspace_id, settings);
    }

    void set_workspace_layout(
        const std::string& workspace_id,
        const std::vector<DisplayLayoutEntry>& layout) override {
        workspaces_->set_layout(workspace_id, layout);
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

    /// @brief Hook control-channel callbacks. Connect / disconnect
    /// transitions log to stderr so the TCP round-trip is visible
    /// during dev. Inbound mouse frames are decoded and handed to
    /// the local @ref IInputBackend — Phase B injects raw global
    /// coords; Phase C will translate them into the local
    /// monitor's coordinate space via the cursor router.
    void wire_control_channel_callbacks() {
        if (!control_channel_) return;
        control_channel_->set_callbacks(
            [this](const std::string& peer, const control::InboundFrame& f) {
                switch (f.type) {
                    case control::MessageType::Heartbeat:
                        break;
                    case control::MessageType::MouseMoveAbs: {
                        // Phase C: cursor stays local on the
                        // active peer; we never receive raw move
                        // events anymore, only Handoffs. Logged
                        // for debug if a peer still uses Phase B.
                        std::fprintf(stderr,
                                     "control: %s legacy MouseMoveAbs (ignored)\n",
                                     peer.c_str());
                        break;
                    }
                    case control::MessageType::Handoff: {
                        auto m = control::decode_handoff(
                            f.payload.data(), f.payload.size());
                        if (m && cursor_router_) {
                            std::fprintf(stderr,
                                         "control: %s handoff @ (%d, %d)\n",
                                         peer.c_str(), m->entry_x, m->entry_y);
                            cursor_router_->on_remote_handoff(
                                peer, m->entry_x, m->entry_y);
                            // We're the active cursor source again —
                            // surface the cursor.
                            sync_cursor_visibility_locked();
                        }
                        break;
                    }
                    case control::MessageType::MouseButton: {
                        auto m = control::decode_mouse_button(
                            f.payload.data(), f.payload.size());
                        if (m && input_backend_) {
                            const auto btn = (m->button >= 1 && m->button <= 3)
                                ? static_cast<input::MouseButton>(m->button)
                                : input::MouseButton::Left;
                            input_backend_->inject_mouse_button(btn, m->pressed);
                        }
                        break;
                    }
                    case control::MessageType::MouseScroll: {
                        auto m = control::decode_mouse_scroll(
                            f.payload.data(), f.payload.size());
                        if (m && input_backend_) {
                            input_backend_->inject_mouse_scroll(m->dx, m->dy);
                        }
                        break;
                    }
                    case control::MessageType::MouseRel: {
                        // Dormant peer is forwarding the user's
                        // mouse motion to us. We hand it to the
                        // cursor router rather than injecting
                        // directly: the router updates an
                        // internal tracked workspace position
                        // (used for edge detection) and warps
                        // the OS cursor to follow. Tracking
                        // separately from the OS cursor is what
                        // keeps touchpad-driver phantom
                        // corrections from bouncing the cursor
                        // back to the source — Barrier's client-
                        // side model.
                        auto m = control::decode_mouse_rel(
                            f.payload.data(), f.payload.size());
                        if (!m || !cursor_router_) break;
                        std::fprintf(stderr,
                                     "rel: from=%s d=(%d,%d)\n",
                                     peer.c_str(), m->dx, m->dy);
                        cursor_router_->apply_remote_delta(m->dx, m->dy);
                        break;
                    }
                    case control::MessageType::KeyEvent: {
                        auto m = control::decode_key_event(
                            f.payload.data(), f.payload.size());
                        if (m && input_backend_) {
                            std::fprintf(stderr,
                                         "control: %s key sc=0x%x %s\n",
                                         peer.c_str(), m->scancode,
                                         m->pressed ? "down" : "up");
                            input_backend_->inject_key(m->scancode,
                                                        m->pressed);
                        }
                        break;
                    }
                    default:
                        std::fprintf(stderr,
                                     "control: %s frame type 0x%04x len %zu\n",
                                     peer.c_str(),
                                     static_cast<unsigned>(f.type),
                                     f.payload.size());
                        break;
                }
            },
            [this](const std::string& peer) {
                std::fprintf(stderr, "control: connected → %s\n",
                             peer.c_str());
                {
                    std::lock_guard lk(connected_peers_m_);
                    connected_peers_.insert(peer);
                }
                update_cursor_poller_state();
                refresh_cursor_router_state();
            },
            [this](const std::string& peer) {
                std::fprintf(stderr, "control: disconnected → %s\n",
                             peer.c_str());
                {
                    std::lock_guard lk(connected_peers_m_);
                    connected_peers_.erase(peer);
                }
                update_cursor_poller_state();
                refresh_cursor_router_state();
            });
    }

    /// @brief Build the local cursor poller + router. The poller
    /// runs only while we have at least one connected peer
    /// (toggled by @ref update_cursor_poller_state on connect /
    /// disconnect events) — zero overhead in solo mode. The
    /// router consumes poller samples and only sends a frame on
    /// edge crossings, so the wire stays quiet between handoffs.
    void wire_cursor_poller() {
        if (!input_backend_) return;
        cursor_router_ = std::make_unique<CursorRouter>(
            local_machine_id_,
            [this](const std::string& target,
                    std::int32_t entry_x, std::int32_t entry_y) {
                std::fprintf(stderr,
                             "router: handoff → %s @ (%d, %d)\n",
                             target.c_str(), entry_x, entry_y);
                send_handoff(target, entry_x, entry_y);
            },
            [this](std::int32_t x, std::int32_t y) {
                if (input_backend_) {
                    last_injected_x_.store(x, std::memory_order_release);
                    last_injected_y_.store(y, std::memory_order_release);
                    input_backend_->inject_mouse_move(x, y);
                }
            },
            [this](const std::string& target,
                    std::int32_t dx, std::int32_t dy) {
                send_mouse_rel(target, dx, dy);
            });
        // Default to active — any peer's first edge crossing
        // claims the cursor; receivers go dormant on Handoff.

        cursor_poller_ = std::make_unique<input::CursorPoller>(
            *input_backend_,
            [this](std::int32_t x, std::int32_t y) {
                if (cursor_router_) cursor_router_->on_local_cursor_move(x, y);
            },
            [this](input::MouseButton btn, bool pressed) {
                // Forward button transitions to the active peer
                // while we're dormant. Active-peer clicks are
                // handled locally by the OS, so we never
                // forward from the active path.
                if (!cursor_router_ || !control_channel_) return;
                const auto target = cursor_router_->forward_target();
                if (target.empty()) return;
                control::MouseButtonMessage m;
                m.button  = static_cast<std::uint8_t>(btn);
                m.pressed = pressed;
                const auto body = control::encode_mouse_button(m);
                control_channel_->send(target,
                                        control::MessageType::MouseButton,
                                        body.data(), body.size());
            });
    }

    /// @brief Start / stop the poller based on live peer count.
    void update_cursor_poller_state() {
        if (!cursor_poller_) return;
        bool any_peer = false;
        {
            std::lock_guard lk(connected_peers_m_);
            any_peer = !connected_peers_.empty();
        }
        if (any_peer) cursor_poller_->start();
        else          cursor_poller_->stop();
    }

    /// @brief Send a Handoff frame to @p target. Called from the
    /// cursor router when our cursor crosses an outer edge mapped
    /// to that peer's display.
    void send_handoff(const std::string& target,
                       std::int32_t entry_x,
                       std::int32_t entry_y) {
        if (!control_channel_ || target.empty()) return;
        control::HandoffMessage m;
        m.entry_x = entry_x;
        m.entry_y = entry_y;
        const auto body = control::encode_handoff(m);
        control_channel_->send(target, control::MessageType::Handoff,
                                body.data(), body.size());
        // Cursor visibility follows the router's active flag:
        // checked peers go dormant + hide cursor; unchecked
        // peers stay active (cursor visible, locally controlled)
        // even after firing a return handoff.
        sync_cursor_visibility_locked();
    }

    /// @brief Sync the local cursor's visibility to the router's
    /// active flag — visible when active, hidden when dormant.
    /// (Input-grab is intentionally NOT enabled here — the LL
    /// mouse/keyboard hooks on Windows were swallowing events
    /// before RawInput could see them, which broke forwarding
    /// in both directions. Local clicks/keys can still leak to
    /// the dormant peer's apps for now; we'll bring grab back
    /// via a different mechanism after forwarding is solid.)
    void sync_cursor_visibility_locked() {
        if (!input_backend_ || !cursor_router_) return;
        const bool active = cursor_router_->is_local_active();
        input_backend_->set_cursor_visible(active);
        input_backend_->set_input_grabbed(false, false);
    }

    /// @brief Forward the user's mouse delta to @p target. Called
    /// from the cursor router while we're the dormant peer — the
    /// router pins our local cursor at the handoff edge and emits
    /// per-frame deltas so the receiving peer can drive its cursor.
    void send_mouse_rel(const std::string& target,
                         std::int32_t dx,
                         std::int32_t dy) {
        if (!control_channel_ || target.empty()) return;
        std::fprintf(stderr, "fwd: → %s rel=(%d, %d)\n",
                     target.c_str(), dx, dy);
        control::MouseRelMessage m;
        m.dx = dx;
        m.dy = dy;
        const auto body = control::encode_mouse_rel(m);
        control_channel_->send(target, control::MessageType::MouseRel,
                                body.data(), body.size());
    }

    /// @brief Wire the platform raw-input listener to forwarding
    /// callbacks. While we're dormant, scroll wheel + keyboard
    /// events the user produces locally are forwarded over the
    /// control channel so the active peer can apply them. While
    /// we're active the user's input lands locally as normal —
    /// we never forward in that case.
    void wire_raw_input_capture() {
        if (!input_backend_) return;
        input::IInputBackend::RawInputCallbacks cbs;
        cbs.on_scroll = [this](std::int32_t dx, std::int32_t dy) {
            if (!cursor_router_ || !control_channel_) return;
            const auto target = cursor_router_->forward_target();
            if (target.empty()) return;
            control::MouseScrollMessage m;
            m.dx = dx;
            m.dy = dy;
            const auto body = control::encode_mouse_scroll(m);
            control_channel_->send(target,
                                    control::MessageType::MouseScroll,
                                    body.data(), body.size());
        };
        cbs.on_key = [this](std::uint32_t scancode, bool pressed) {
            if (!cursor_router_ || !control_channel_) return;
            // Keyboard checkbox gate: even if the cursor lives
            // on another peer, only forward our local keystrokes
            // when this PC is in the workspace's keyboard set.
            const bool forwardable =
                cursor_router_->keyboard_forwardable();
            const auto target = cursor_router_->forward_target();
            std::fprintf(stderr,
                         "raw: key sc=0x%x %s "
                         "(forwardable=%d target=%s)\n",
                         scancode, pressed ? "down" : "up",
                         forwardable ? 1 : 0,
                         target.empty() ? "-" : target.c_str());
            if (!forwardable || target.empty()) return;
            control::KeyEventMessage m;
            m.scancode = scancode;
            m.pressed  = pressed;
            const auto body = control::encode_key_event(m);
            control_channel_->send(target,
                                    control::MessageType::KeyEvent,
                                    body.data(), body.size());
        };
        input_backend_->start_raw_capture(std::move(cbs));
    }

    /// @brief Refresh the cursor router's view: strip of peers in
    /// the active workspace + every peer's monitor rects, applying
    /// the user-arranged layout overrides so adjacency follows
    /// what the user dragged in the Layout canvas.
    void refresh_cursor_router_state() {
        if (!cursor_router_) return;

        // Active workspace = the first workspace whose member
        // set contains us. Membership (not input_members) is the
        // canvas filter so a peer with the Input checkbox off
        // can still *receive* the cursor — it just won't INITIATE
        // handoffs from its own mouse, gated separately below.
        std::unordered_set<std::string> ws_members;
        std::unordered_set<std::string> ws_input;
        std::vector<DisplayLayoutEntry> layout_entries;
        if (workspaces_) {
            const auto wss = workspaces_->list();
            for (const auto& ws : wss) {
                if (ws.members.count(local_machine_id_) == 0) continue;
                ws_members     = ws.members;
                ws_input       = ws.input_members;
                layout_entries = ws.layout;
                break;
            }
        }
        // One Input flag per peer drives both the cursor side
        // (initiates handoffs) and the keyboard side (forwards
        // typing while dormant). The cursor_router still takes
        // both as separate args so the unit-tested two-flag
        // contract doesn't change; we just pass the same value
        // for both.
        const bool is_input_member =
            ws_input.count(local_machine_id_) > 0;
        cursor_router_->set_local_member_flags(is_input_member,
                                                 is_input_member);
        std::fprintf(stderr,
                     "router: local input member=%d "
                     "(workspace members=%zu)\n",
                     is_input_member ? 1 : 0,
                     ws_members.size());
        // The router may have just transitioned from dormant
        // back to active (cursor membership revoked while
        // forwarding) — surface the local cursor again.
        sync_cursor_visibility_locked();

        // Monitors: every peer's caps from the mesh CRDT, with
        // the workspace's per-monitor layout entries overriding
        // the local-probe coords. New / un-arranged monitors
        // fall back to whatever the local probe reported.
        std::vector<RouterMonitor> monitors;
        if (mesh_) {
            for (const auto& [_, caps] : mesh_->all_caps()) {
                // Only contribute monitors from peers in the
                // workspace's *member* set — Cursor / Keyboard
                // capability filtering happens later inside the
                // router. A peer with the Cursor checkbox off
                // is still a valid destination; it just can't
                // initiate from its own mouse.
                if (!ws_members.empty()
                    && ws_members.count(caps.machine_id) == 0) {
                    continue;
                }
                for (const auto& d : caps.displays) {
                    RouterMonitor rm;
                    rm.machine_id = d.machine_id;
                    // Display.global_x/y in our model is actually
                    // the announcing peer's OS-local coord — the
                    // local probe doesn't yet know about mesh-
                    // global space. Treat it as local; let the
                    // layout entry override populate the real
                    // global fields if the user has arranged this
                    // monitor.
                    rm.local_x    = d.global_x;
                    rm.local_y    = d.global_y;
                    rm.global_x   = d.global_x;
                    rm.global_y   = d.global_y;
                    rm.width      = d.width;
                    rm.height     = d.height;
                    for (const auto& e : layout_entries) {
                        if (e.machine_id == d.machine_id
                            && e.monitor_id == d.monitor_id) {
                            rm.global_x = e.global_x;
                            rm.global_y = e.global_y;
                            break;
                        }
                    }
                    monitors.push_back(rm);
                }
            }
        }
        cursor_router_->set_monitors(std::move(monitors));
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

    /// @brief Snapshot the workspace catalogue (incl. tombstones)
    /// for the next announce datagram.
    std::vector<net::AnnounceWorkspace>
    wire_workspaces_for_announce() const {
        std::vector<net::AnnounceWorkspace> out;
        if (!workspaces_) return out;
        const auto state = workspaces_->wire_state();
        out.reserve(state.size());
        for (const auto& ws : state) {
            net::AnnounceWorkspace w;
            w.id         = ws.id;
            w.name       = ws.name;
            w.version_ns = ws.version_ns;
            w.tombstone  = ws.tombstone;
            auto fill = [](std::vector<std::string>& dst,
                           const std::unordered_set<std::string>& src) {
                dst.reserve(src.size());
                for (const auto& m : src) dst.push_back(m);
                std::sort(dst.begin(), dst.end());
            };
            fill(w.members,           ws.members);
            fill(w.input_members,     ws.input_members);
            fill(w.clipboard_members, ws.clipboard_members);
            w.clipboard_max          = static_cast<std::uint8_t>(ws.clipboard_max);
            w.clipboard_rich         = ws.clipboard_rich;
            w.clipboard_files        = ws.clipboard_files;
            w.cursor_edge_margin     = ws.cursor_edge_margin;
            w.cursor_require_modifier = ws.cursor_require_modifier;
            w.cursor_block_hotkeys   = ws.cursor_block_hotkeys;
            w.auto_unlock            = static_cast<std::uint8_t>(ws.auto_unlock);
            w.layout.reserve(ws.layout.size());
            for (const auto& e : ws.layout) {
                net::AnnounceWorkspace::LayoutEntry le;
                le.machine_id = e.machine_id;
                le.monitor_id = e.monitor_id;
                le.global_x   = e.global_x;
                le.global_y   = e.global_y;
                w.layout.push_back(std::move(le));
            }
            out.push_back(std::move(w));
        }
        return out;
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
            [this](const DiscoveryAnnouncement& a) {
                peer_events_.handle_peer_observed(a);
                // Re-derive the cursor router's monitor list
                // every time we see an announce. The workspace-
                // change callback only fires when LWW actually
                // mutates state, so a stable workspace whose
                // member peers' display caps just arrived
                // wouldn't otherwise refresh the router. Without
                // this refresh, Diana's adjacency search has no
                // adi-pc monitors to find as neighbours and a
                // return-edge handoff silently fails.
                refresh_cursor_router_state();
            },
            [this](const std::string& mid) {
                peer_events_.handle_peer_lost(mid);
                refresh_cursor_router_state();
            });

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

    OrchestratorCallbacks                            callbacks_;
    std::chrono::steady_clock::time_point            start_time_;

    std::string                                      local_machine_id_;
    std::string                                      local_display_name_;

    std::unique_ptr<ILocalProbeAdapter>              local_probe_;
    std::unique_ptr<IMeshCrdt>                       mesh_;
    // Workspaces is constructed before discovery so the
    // discovery config's workspaces_provider can capture
    // &*workspaces_ safely (member-init order = declaration
    // order in C++).
    std::unique_ptr<IWorkspaceManager>               workspaces_;
    // Control-channel listen port (set by start_control_channel
    // helper during init). Declared before control_channel_ so
    // its address is bindable inside the helper call; declared
    // before discovery_ so the LAN announce config reads its
    // post-helper value, not the pre-init zero.
    std::uint16_t                                    control_port_ = 0;
    std::unique_ptr<control::IControlChannel>        control_channel_;
    std::unique_ptr<input::IInputBackend>            input_backend_;
    /// @brief State machine that decides whether a local cursor
    /// move should fire a Handoff (active peer at edge) or be
    /// ignored (dormant peer). Pure logic — owns no transport.
    std::unique_ptr<CursorRouter>                    cursor_router_;
    /// @brief Polling thread that samples the local cursor and
    /// pipes samples into the router. Idle while no peers are
    /// connected — see update_cursor_poller_state.
    std::unique_ptr<input::CursorPoller>             cursor_poller_;
    /// @brief Live set of peers reachable over the control
    /// channel, kept in sync via the on_connected /
    /// on_disconnected callbacks.
    mutable std::mutex                               connected_peers_m_;
    std::unordered_set<std::string>                  connected_peers_;
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
    std::atomic<std::uint64_t>                       identify_counter_{0};

    /// @brief Cursor-arbitration state. The poller broadcasts
    /// only when the local cursor genuinely moved by user
    /// action — these atomics let us distinguish that from a
    /// move our own inject path just produced.
    std::atomic<std::int32_t>                        last_injected_x_{INT32_MIN};
    std::atomic<std::int32_t>                        last_injected_y_{INT32_MIN};
    std::atomic<std::int64_t>                        last_remote_move_ms_{0};

    /// @brief Translates discovery events into mesh + UI side
    /// effects. Borrows references to several members above —
    /// declared last so its references are valid by the time its
    /// constructor body runs (member-init order matches declaration
    /// order in C++).
    PeerEventHandler                                 peer_events_;
};

}  // namespace

std::unique_ptr<IOrchestrator>
make_mock(const OrchestratorCallbacks& callbacks) {
    return std::make_unique<FacadeOrchestrator>(callbacks);
}

}  // namespace unio_ui::orchestrator
