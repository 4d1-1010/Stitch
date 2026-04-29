/// @file orchestrator.cpp
/// @brief Façade implementation: composes the seven internal
/// sub-modules, exposes the @ref IOrchestrator public surface,
/// and drives a worker thread that simulates the mesh timeline
/// (local probe → discovery → pairing → signed-in).

#include "orchestrator/orchestrator.hpp"

#include "orchestrator/clipboard_backend.hpp"
#include "orchestrator/clipboard_monitor.hpp"
#include "orchestrator/control/control_channel.hpp"
#include "orchestrator/control/protocol.hpp"
#include "orchestrator/file_transfer_receiver.hpp"
#include "orchestrator/file_transfer_sender.hpp"
#include "orchestrator/crypto.hpp"
#include "orchestrator/cursor_router.hpp"
#include "orchestrator/input/cursor_poller.hpp"
#include "orchestrator/input/input_backend.hpp"
#include "orchestrator/input/polled_motion_forwarder.hpp"
#include "orchestrator/local_probe.hpp"
#include "orchestrator/net/lan_discovery.hpp"
#include "orchestrator/peer_events.hpp"

#include "platform/transfer_overlay.hpp"

#include "mock/factories.hpp"

#include <cstdio>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
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
          // Sibling channel exclusively for file-transfer frames.
          // Identical implementation, distinct listen port, so
          // chunks don't share a send mutex with cursor / key /
          // clipboard-control frames. Bound port written into
          // data_port_ before the discovery config below reads
          // it (member-init order = declaration order).
          data_channel_(start_control_channel(local_machine_id_,
                                                data_port_)),
          input_backend_(input::make_default_input_backend()),
          clipboard_backend_(make_default_clipboard_backend()),
          discovery_(net::make_lan_discovery(net::LanDiscoveryConfig{
              local_machine_id_,
              local_display_name_,
              control_port_,          // Real bound port from above.
              data_port_,             // Sibling data-channel port.
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
                       data_channel_.get(),
                       callbacks_, access_authorized_,
                       peers_m_, peers_) {
        wire_sub_modules();
        publish_local_caps();
        if (input_backend_) input_backend_->open();
        if (clipboard_backend_) clipboard_backend_->open();
        wire_control_channel_callbacks();
        wire_data_channel_callbacks();
        wire_cursor_poller();
        wire_raw_input_capture();
        wire_clipboard_monitor();
        wire_file_transfer_receiver();
        wire_transfer_overlay();
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
        if (cursor_poller_)     cursor_poller_->stop();
        if (clipboard_monitor_) clipboard_monitor_->stop();
        // Tear the overlay down before the senders/receiver so
        // the refresh thread can't race a freed @ref file_senders_
        // map on its way out.
        if (transfer_overlay_)  transfer_overlay_->stop();
        discovery_->stop();
        if (control_channel_)   control_channel_->stop();
        if (data_channel_)      data_channel_->stop();
        if (input_backend_)     input_backend_->close();
        if (clipboard_backend_) clipboard_backend_->close();
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
                            // surface the cursor. The clipboard
                            // pull is deferred to the actual Ctrl+V
                            // keystroke (see @ref intercept_ctrl_v),
                            // not triggered on cursor arrival —
                            // otherwise a flyby through this peer
                            // would speculatively pull bytes that
                            // never end up pasted.
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
                            if (intercept_ctrl_v(m->scancode,
                                                  m->pressed)) {
                                break;
                            }
                            input_backend_->inject_key(m->scancode,
                                                        m->pressed);
                        }
                        break;
                    }
                    case control::MessageType::ClipboardUpdate: {
                        auto m = control::decode_clipboard(
                            f.payload.data(), f.payload.size());
                        if (m) handle_clipboard_inbound(peer, *m);
                        break;
                    }
                    case control::MessageType::ClipboardLatest: {
                        auto m = control::decode_clipboard_latest(
                            f.payload.data(), f.payload.size());
                        if (m) handle_clipboard_latest_inbound(peer, *m);
                        break;
                    }
                    case control::MessageType::ClipboardFetch: {
                        auto m = control::decode_clipboard_fetch(
                            f.payload.data(), f.payload.size());
                        if (m) handle_clipboard_fetch_inbound(peer, *m);
                        break;
                    }
                    // File-transfer frames now arrive on the
                    // sibling data channel so chunks don't
                    // queue cursor / keyboard messages behind
                    // the per-link send mutex on this channel.
                    // See @ref wire_data_channel_callbacks.
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

    /// @brief Wire the sibling data-channel callbacks. The data
    /// channel only carries the four file-transfer message
    /// types; every other type is logged + dropped so a stray
    /// frame on this socket can't poison cursor / keyboard
    /// state. Connect / disconnect events update no global
    /// peer-set state — the control channel's connected_peers_
    /// is the authoritative live-link source.
    void wire_data_channel_callbacks() {
        if (!data_channel_) return;
        data_channel_->set_callbacks(
            [this](const std::string& peer,
                    const control::InboundFrame& f) {
                switch (f.type) {
                    case control::MessageType::FileTransferStart: {
                        auto m = control::decode_file_start(
                            f.payload.data(), f.payload.size());
                        if (m && file_transfer_receiver_) {
                            file_transfer_receiver_->on_start(peer, *m);
                        }
                        break;
                    }
                    case control::MessageType::FileChunk: {
                        auto m = control::decode_file_chunk(
                            f.payload.data(), f.payload.size());
                        if (m && file_transfer_receiver_) {
                            file_transfer_receiver_->on_chunk(peer, *m);
                        }
                        break;
                    }
                    case control::MessageType::FileTransferEnd: {
                        auto m = control::decode_file_end(
                            f.payload.data(), f.payload.size());
                        if (m && file_transfer_receiver_) {
                            file_transfer_receiver_->on_end(peer, *m);
                        }
                        break;
                    }
                    case control::MessageType::FileTransferCancel: {
                        auto m = control::decode_file_cancel(
                            f.payload.data(), f.payload.size());
                        if (!m) break;
                        if (file_transfer_receiver_) {
                            file_transfer_receiver_->on_cancel(peer, *m);
                        }
                        std::lock_guard lk(file_senders_m_);
                        auto it = file_senders_.find(m->transfer_id);
                        if (it != file_senders_.end() && it->second) {
                            std::fprintf(stderr,
                                         "file_xfer: peer %s "
                                         "cancelled tx=%llu — "
                                         "stopping sender\n",
                                         peer.c_str(),
                                         static_cast<unsigned long long>(
                                             m->transfer_id));
                            it->second->cancel("receiver cancelled");
                        }
                        break;
                    }
                    default:
                        std::fprintf(stderr,
                                     "data: %s unexpected frame "
                                     "type 0x%04x len %zu\n",
                                     peer.c_str(),
                                     static_cast<unsigned>(f.type),
                                     f.payload.size());
                        break;
                }
            },
            [](const std::string& peer) {
                std::fprintf(stderr,
                             "data: connected → %s\n",
                             peer.c_str());
            },
            [](const std::string& peer) {
                std::fprintf(stderr,
                             "data: disconnected → %s\n",
                             peer.c_str());
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
                    // Re-sync the polled-cursor reference used
                    // by the raw on_motion forwarder. After we
                    // warp the OS cursor (handoff entry, pin
                    // warp, tracked re-warp), polled jumps to
                    // (x, y); without this re-sync the next raw
                    // event would compute a huge bogus delta
                    // (current_polled - stale last_polled) and
                    // forward it to the active peer, racing
                    // their cursor across the screen.
                    // Invalidate the forwarder's polled
                    // reference so the next on_hardware_motion
                    // re-baselines against the OS cursor
                    // position it actually reads. On X11
                    // there's a small race between the warp
                    // (XTestFakeMotionEvent) landing and the
                    // next XQueryPointer reflecting it —
                    // computing (current_polled - warp_target)
                    // before the warp lands gives a huge
                    // spurious delta that we'd forward to the
                    // active peer, snapping its cursor across
                    // the screen.
                    if (motion_forwarder_) {
                        motion_forwarder_->invalidate_polled_reference();
                    }
                }
            },
            [this](const std::string& target,
                    std::int32_t dx, std::int32_t dy) {
                send_mouse_rel(target, dx, dy);
            },
            [this](std::int32_t dx, std::int32_t dy) {
                // Remote peer is forwarding their mouse motion
                // and we're the locally active source (cursor
                // home here). Apply the delta as a plain
                // relative cursor move; the active poll path
                // sees the new polled position next tick and
                // runs edge detection from there, just as if
                // our own user had moved the mouse.
                if (!input_backend_) return;
                std::int32_t cx = 0, cy = 0;
                if (!input_backend_->get_cursor_pos(cx, cy)) return;
                input_backend_->inject_mouse_move(cx + dx, cy + dy);
            });
        // Default to active — any peer's first edge crossing
        // claims the cursor; receivers go dormant on Handoff.

        // Button transitions are forwarded from the raw-capture
        // layer (cbs.on_button in wire_raw_input_capture) — that
        // path sees button events at the device level before the
        // local grab (Win32 LL hook / X11 XGrabButton)
        // suppresses them from reaching apps. The poller's
        // get_button_mask() reads OS-pollable state which misses
        // events the grab swallows, so wiring it here would
        // silently drop clicks while dormant.
        cursor_poller_ = std::make_unique<input::CursorPoller>(
            *input_backend_,
            [this](std::int32_t x, std::int32_t y) {
                if (cursor_router_) cursor_router_->on_local_cursor_move(x, y);
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

    /// @brief Sync the local cursor's visibility + input grab to
    /// the router's active flag. Active peer: cursor visible, no
    /// grab. Dormant peer: cursor hidden, grab pointer + keyboard
    /// (gated by the workspace's per-member input flags) so local
    /// clicks / keystrokes don't fire twice — once locally and
    /// once on the active peer via forwarding.
    void sync_cursor_visibility_locked() {
        if (!input_backend_ || !cursor_router_) return;
        const bool active = cursor_router_->is_local_active();
        input_backend_->set_cursor_visible(active);
        // Grab local pointer + keyboard while we're dormant
        // (cursor lives on another peer): without it, the user
        // clicking or typing on this PC fires both locally
        // (because the OS cursor is here, just hidden) and
        // remotely (forwarded over the wire), so the click
        // executes twice. Only meaningful for input-member
        // peers — non-members never go dormant in the first
        // place (they're either local-active or active+tracked
        // receiving a visit).
        const bool dormant = !active;
        const bool grab_ptr = dormant
                            && cursor_router_->is_cursor_member();
        const bool grab_kbd = dormant
                            && cursor_router_->is_keyboard_member();
        input_backend_->set_input_grabbed(grab_ptr, grab_kbd);
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
        // Build the polled-motion forwarder lazily here — it
        // needs both input_backend_ and cursor_router_, which
        // are constructed earlier but not from the same init
        // list. Owning it via unique_ptr keeps the orchestrator
        // declaration order independent of the forwarder's
        // lifetime.
        if (input_backend_ && cursor_router_) {
            motion_forwarder_ =
                std::make_unique<input::PolledMotionForwarder>(
                    *input_backend_, *cursor_router_,
                    [this](const std::string& target,
                            std::int32_t dx, std::int32_t dy) {
                        send_mouse_rel(target, dx, dy);
                    });
        }
        cbs.on_motion = [this](std::int32_t dx, std::int32_t dy) {
            // Two motion paths:
            //   * Backend-grabbed (Linux EVIOCGRAB): kernel input
            //     is exclusively ours; the OS cursor is frozen.
            //     The polled-cursor forwarder would see zero
            //     deltas, so we forward the raw delta directly.
            //   * Backend not grabbed (Windows LL hook lets motion
            //     through; Linux while active): polled-cursor
            //     forwarder handles edge pin-warp + tracked sync.
            if (input_backend_ && input_backend_->is_input_grabbed()) {
                if (!cursor_router_ || !control_channel_) return;
                cursor_router_->note_local_hardware_motion();
                const auto target = cursor_router_->forward_target();
                if (target.empty()) return;
                send_mouse_rel(target, dx, dy);
                return;
            }
            if (motion_forwarder_) {
                motion_forwarder_->on_hardware_motion(dx, dy);
            }
        };
        cbs.on_button = [this](input::MouseButton btn, bool pressed) {
            // Button forwarding lives on the raw-capture path
            // because the dormant peer's local-input grab
            // (Win32 LL hook / X11 XGrabButton) suppresses
            // button events from reaching the OS-level pollers
            // — RawInput / XInput2 see the device-level event
            // before any of that suppression takes effect.
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
        };
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

    /// @brief Translate the workspace's @c clipboard_max enum
    /// into a byte cap. ClipboardLimit::Unlimited returns 0
    /// (= no cap), matching the convention the rest of the
    /// pipeline uses ("0 means unbounded").
    static std::size_t clipboard_max_bytes(ClipboardLimit lim) {
        switch (lim) {
            case ClipboardLimit::Kb100:     return 100  * 1024;
            case ClipboardLimit::Mb1:       return 1    * 1024 * 1024;
            case ClipboardLimit::Mb5:       return 5    * 1024 * 1024;
            case ClipboardLimit::Mb10:      return 10   * 1024 * 1024;
            case ClipboardLimit::Unlimited: return 0;
        }
        return 0;
    }

    /// @brief Look up the active workspace's clipboard policy
    /// for the local peer. Returns the workspace member set,
    /// whether we're in @c clipboard_members (= allowed to
    /// broadcast our local clipboard), the per-workspace
    /// "Include rich text/images" toggle, and the per-text
    /// size cap. When no workspace contains us, every flag
    /// returns its restrictive default.
    struct ClipboardPolicy {
        std::unordered_set<std::string> ws_members;
        bool                            outbound_allowed = false;
        bool                            allow_rich       = false;
        bool                            allow_files      = false;
        std::size_t                     max_text_bytes   = 0;
    };
    ClipboardPolicy current_clipboard_policy() const {
        ClipboardPolicy out;
        if (!workspaces_) return out;
        const auto wss = workspaces_->list();
        for (const auto& ws : wss) {
            if (ws.members.count(local_machine_id_) == 0) continue;
            out.ws_members       = ws.members;
            out.outbound_allowed =
                ws.clipboard_members.count(local_machine_id_) > 0;
            out.allow_rich       = ws.clipboard_rich;
            out.allow_files      = ws.clipboard_files;
            out.max_text_bytes   = clipboard_max_bytes(ws.clipboard_max);
            break;
        }
        return out;
    }

    /// @brief Build the local clipboard monitor. The monitor
    /// polls the platform clipboard at 500 ms cadence, fires
    /// on_change for user-initiated changes (echo-suppressed
    /// against just-injected inbound updates), and we apply
    /// the workspace gate before broadcasting.
    void wire_clipboard_monitor() {
        if (!clipboard_backend_) return;
        clipboard_monitor_ = std::make_unique<ClipboardMonitor>(
            clipboard_backend_.get(),
            [this](const ClipboardData& data) {
                capture_local_clipboard(data);
            },
            [this](const ClipboardFiles& files) {
                capture_local_files(files);
            });
        clipboard_monitor_->start();
    }

    /// @brief Build the cross-PC file-transfer receiver. The
    /// per-process root for temp dirs lives under
    /// std::filesystem::temp_directory_path() so it's
    /// $TMPDIR / /tmp on Linux and %TEMP% on Windows. Each
    /// transfer materialises into a 16-hex-char subdir
    /// keyed by transfer_id.
    /// @brief Resolve a peer's machine_id to its display name
    /// for the overlay's "Sending → <name>" caption. Falls
    /// back to the machine_id itself so an unmapped peer still
    /// shows something readable.
    std::string peer_display_name_for(const std::string& mid) const {
        std::lock_guard lk(peers_m_);
        auto it = peers_.find(mid);
        if (it == peers_.end() || it->second.display_name.empty()) {
            return mid;
        }
        return it->second.display_name;
    }

    /// @brief Compose the human-friendly selection summary the
    /// overlay shows alongside the peer name. Mirrors the
    /// "myfolder" / "3 files" convention used in the OS-native
    /// paste menus.
    static std::string summarize_roots(
            const std::vector<std::string>& roots) {
        if (roots.empty()) return "(no files)";
        if (roots.size() == 1) return roots[0];
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "%zu items", roots.size());
        return buf;
    }

    /// @brief Build the platform-native overlay window and wire
    /// its progress fetcher to a snapshot of the active sender +
    /// receiver state. The fetcher runs on the overlay's own
    /// refresh thread; it must not touch any orchestrator state
    /// that isn't mutex-guarded.
    void wire_transfer_overlay() {
        transfer_overlay_ = platform::make_transfer_overlay();
        if (!transfer_overlay_) return;
        transfer_overlay_->set_cancel_handler(
            [this](std::uint64_t transfer_id) {
                cancel_transfer(transfer_id);
            });
        transfer_overlay_->set_progress_fetcher(
            [this]() -> std::vector<platform::TransferOverlayItem> {
                std::vector<platform::TransferOverlayItem> out;

                // Outbound senders. Done / cancelled rows linger
                // for ~1 s before the on_finished cleanup erases
                // them — the overlay's 10 Hz poll would otherwise
                // miss a short-lived transfer entirely.
                {
                    std::lock_guard lk(file_senders_m_);
                    for (const auto& [id, s] : file_senders_) {
                        if (!s) continue;
                        const auto p = s->progress();
                        platform::TransferOverlayItem row;
                        row.direction        = platform::TransferOverlayItem
                                                   ::Direction::Sending;
                        row.transfer_id      = id;
                        row.peer_name        = "peer";
                        row.label            = p.cancelled ? "cancelled"
                                              : p.failed   ? "failed"
                                              : p.done     ? "done"
                                              : "transfer";
                        row.bytes_done       = p.bytes_sent;
                        row.bytes_total      = p.bytes_total;
                        row.current_file_idx = p.current_file_idx;
                        row.file_count       = p.file_count;
                        out.push_back(std::move(row));
                    }
                }

                // Inbound transfers.
                if (file_transfer_receiver_) {
                    auto inbound = file_transfer_receiver_
                                     ->progress_snapshot();
                    for (auto& p : inbound) {
                        platform::TransferOverlayItem row;
                        row.direction        = platform::TransferOverlayItem
                                                   ::Direction::Receiving;
                        row.transfer_id      = p.transfer_id;
                        row.peer_name        = peer_display_name_for(
                                                   p.source_machine);
                        row.label            = summarize_roots(
                                                   p.selection_roots);
                        row.bytes_done       = p.bytes_received;
                        row.bytes_total      = p.bytes_total;
                        row.current_file_idx = p.current_file_idx;
                        row.file_count       = p.file_count;
                        out.push_back(std::move(row));
                    }
                }
                return out;
            });
        transfer_overlay_->start();
    }

    void wire_file_transfer_receiver() {
        if (!clipboard_backend_) return;
        // Stage on the same filesystem the user is most likely
        // to paste into (their home tree on Linux, profile
        // drive on Windows). The OS-side paste then degenerates
        // to an O(1) rename instead of a full copy — see
        // @ref publish_to_clipboard_locked, which sets the
        // clipboard's drop-effect to MOVE so the file manager
        // *moves* the staged files into the destination.
        std::error_code ec;
        std::filesystem::path root;
#if defined(_WIN32)
        // %TEMP% is on the user's local profile drive, same FS
        // as Documents / Desktop / Downloads in nearly every
        // setup. Cross-drive pastes still fall back to a copy
        // (no way around that without write-direct-to-dest).
        root = std::filesystem::temp_directory_path(ec);
        if (ec) root = std::filesystem::path(".");
#else
        // ~/.cache/unio-clipboard — same filesystem as ~/Desktop
        // etc. Beats /tmp (often tmpfs on a separate FS, which
        // would force the OS paste to do a copy + delete
        // instead of a rename).
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        const char* home = std::getenv("HOME");
        if (xdg && *xdg) {
            root = std::filesystem::path(xdg);
        } else if (home && *home) {
            root = std::filesystem::path(home) / ".cache";
        } else {
            root = std::filesystem::temp_directory_path(ec);
            if (ec) root = std::filesystem::path("/tmp");
        }
#endif
        root /= "unio-clipboard";
        file_transfer_receiver_ =
            std::make_unique<FileTransferReceiver>(
                clipboard_backend_.get(),
                clipboard_monitor_.get(),
                std::move(root),
                [this]() {
                    // Files just landed on the OS clipboard;
                    // if the user pressed Ctrl+V earlier and
                    // we deferred V until the bytes arrived,
                    // synthesise the keystroke now.
                    release_pending_paste();
                });
    }

    /// @brief Cache the local file selection for later lazy
    /// pull. Files don't ride the wire until a remote peer
    /// asks for them via @ref ClipboardFetchMessage; here we
    /// just snapshot what the user copied + bump the announce
    /// counter so peers know there's something fresher
    /// available.
    void capture_local_files(const ClipboardFiles& files) {
        if (files.empty()) return;
        const auto policy = current_clipboard_policy();
        // Disabled-clipboard peers don't advertise. The local
        // clipboard still works for same-PC paste — we just
        // stay silent on the wire so other peers won't try
        // to fetch from us.
        if (!policy.outbound_allowed) return;
        if (!policy.allow_files)     return;

        std::lock_guard lk(local_clip_m_);
        // File selection clears any cached text/image (mirrors
        // the X11 backend's "files + text are mutually
        // exclusive" rule).
        local_text_      = {};
        local_files_     = files;
        ++local_last_copy_t_;
        announce_local_clipboard_locked(/*has_text=*/false,
                                          /*has_html=*/false,
                                          /*has_image=*/false,
                                          /*has_files=*/true);
    }

    /// @brief Cache the local text/html/image selection. Same
    /// shape as @ref capture_local_files: announce-only on
    /// the wire, content stays local until pulled.
    void capture_local_clipboard(const ClipboardData& data) {
        const auto policy = current_clipboard_policy();
        if (!policy.outbound_allowed) return;

        ClipboardData stored;
        // Plain-text gate: oversize text stays local-only and
        // never crosses the wire, even on a fetch reply.
        if (policy.max_text_bytes == 0
            || data.text.size() <= policy.max_text_bytes) {
            stored.text = data.text;
        } else {
            std::fprintf(stderr,
                         "clipboard: text %zu bytes exceeds "
                         "workspace limit %zu — text dropped\n",
                         data.text.size(), policy.max_text_bytes);
        }
        if (policy.allow_rich) {
            stored.html        = data.html;
            stored.image_mime  = data.image_mime;
            stored.image_bytes = data.image_bytes;
        }
        if (stored.empty()) return;

        std::lock_guard lk(local_clip_m_);
        // Text/image clears any cached file selection — we
        // can't have both as the "current" copy.
        local_files_ = {};
        local_text_  = std::move(stored);
        ++local_last_copy_t_;
        announce_local_clipboard_locked(
            /*has_text=*/!local_text_.text.empty(),
            /*has_html=*/!local_text_.html.empty(),
            /*has_image=*/!local_text_.image_bytes.empty(),
            /*has_files=*/false);
    }

    /// @brief Broadcast a tiny @ref ClipboardLatestMessage to
    /// every workspace peer announcing that we have fresh
    /// content. No payload bytes — receivers pull on demand.
    /// Caller must hold @ref local_clip_m_.
    void announce_local_clipboard_locked(bool has_text,
                                           bool has_html,
                                           bool has_image,
                                           bool has_files) {
        if (!control_channel_) return;
        const auto policy = current_clipboard_policy();
        if (!policy.outbound_allowed) return;

        control::ClipboardLatestMessage m;
        m.source_machine = local_machine_id_;
        m.last_copy_t    = local_last_copy_t_;
        m.has_text       = has_text;
        m.has_html       = has_html;
        m.has_image      = has_image;
        m.has_files      = has_files;
        const auto body  = control::encode_clipboard_latest(m);

        std::vector<std::string> targets;
        {
            std::lock_guard lk(connected_peers_m_);
            targets.reserve(connected_peers_.size());
            for (const auto& mid : connected_peers_) {
                if (policy.ws_members.count(mid) == 0) continue;
                targets.push_back(mid);
            }
        }
        std::fprintf(stderr,
                     "clipboard: announce t=%llu "
                     "(text=%d html=%d image=%d files=%d) "
                     "→ %zu peers\n",
                     static_cast<unsigned long long>(m.last_copy_t),
                     has_text, has_html, has_image, has_files,
                     targets.size());
        for (const auto& mid : targets) {
            control_channel_->send(mid,
                                    control::MessageType::ClipboardLatest,
                                    body.data(), body.size());
        }
    }

    /// @brief Inbound @ref ClipboardLatestMessage from a peer.
    /// Records the announcement; if the cursor lives here right
    /// now, fire a fetch immediately so a paste on this peer
    /// finds the fresh content without first having to bounce
    /// the cursor.
    void handle_clipboard_latest_inbound(
            const std::string& peer,
            const control::ClipboardLatestMessage& m) {
        const auto policy = current_clipboard_policy();
        if (policy.ws_members.count(peer) == 0) return;
        std::fprintf(stderr,
                     "clipboard: latest from %s t=%llu "
                     "(text=%d html=%d image=%d files=%d)\n",
                     peer.c_str(),
                     static_cast<unsigned long long>(m.last_copy_t),
                     m.has_text, m.has_html, m.has_image, m.has_files);
        {
            std::lock_guard lk(remote_clip_m_);
            // Always overwrite — most-recently-received peer
            // wins, matching the user's "last Ctrl+C across
            // the workspace" rule.
            latest_remote_source_ = peer;
            latest_remote_t_      = m.last_copy_t;
            latest_remote_flags_  = (m.has_text  ? 0x01 : 0)
                                  | (m.has_html  ? 0x02 : 0)
                                  | (m.has_image ? 0x04 : 0)
                                  | (m.has_files ? 0x08 : 0);
        }
        // Deliberately no immediate fetch here — the actual
        // pull is gated on the user's Ctrl+V keystroke (see
        // @ref intercept_ctrl_v). This keeps a passing-through
        // cursor / a stale announce arriving while idle from
        // pre-fetching bytes the user never actually pastes.
    }

    /// @brief Send a @ref ClipboardFetchMessage to the peer
    /// holding the freshest known announce, IF we haven't
    /// already pulled this exact (source, t) tuple. Idempotent
    /// — safe to call from multiple triggers (handoff arrival,
    /// announce while active).
    void maybe_fetch_clipboard(const char* trigger) {
        if (!control_channel_) return;
        std::string source;
        std::uint64_t t = 0;
        {
            std::lock_guard lk(remote_clip_m_);
            if (latest_remote_source_.empty()) return;
            // File content uses MOVE-on-paste semantics so the
            // staged files are consumed on the first paste; a
            // repeat Ctrl+V on the same (source, t) needs a
            // fresh fetch to re-stage. Text and image stay on
            // the OS clipboard and can be pasted repeatedly,
            // so we keep the dedupe for them.
            const bool has_files =
                (latest_remote_flags_ & 0x08) != 0;
            if (!has_files
                && latest_remote_source_ == last_fetched_source_
                && latest_remote_t_   == last_fetched_t_) {
                return;
            }
            source = latest_remote_source_;
            t      = latest_remote_t_;
            last_fetched_source_ = source;
            last_fetched_t_      = t;
        }
        const auto policy = current_clipboard_policy();
        if (policy.ws_members.count(source) == 0) return;

        control::ClipboardFetchMessage req;
        req.requester_machine    = local_machine_id_;
        req.expected_last_copy_t = t;
        const auto body = control::encode_clipboard_fetch(req);
        std::fprintf(stderr,
                     "clipboard: fetch %s t=%llu (trigger=%s)\n",
                     source.c_str(),
                     static_cast<unsigned long long>(t),
                     trigger);
        control_channel_->send(source,
                                control::MessageType::ClipboardFetch,
                                body.data(), body.size());
    }

    /// @brief Inbound @ref ClipboardFetchMessage. Read our
    /// cached local clipboard and reply with content addressed
    /// only to the requester. Stale fetches (asking for an
    /// older @c expected_last_copy_t) are dropped — the
    /// requester will catch up on the next announce.
    void handle_clipboard_fetch_inbound(
            const std::string& requester,
            const control::ClipboardFetchMessage& m) {
        const auto policy = current_clipboard_policy();
        if (policy.ws_members.count(requester) == 0) return;
        if (!policy.outbound_allowed)              return;

        ClipboardData  text_snapshot;
        ClipboardFiles files_snapshot;
        std::uint64_t  current_t = 0;
        {
            std::lock_guard lk(local_clip_m_);
            current_t = local_last_copy_t_;
            text_snapshot  = local_text_;
            files_snapshot = local_files_;
        }
        if (m.expected_last_copy_t != current_t) {
            std::fprintf(stderr,
                         "clipboard: fetch from %s expected t=%llu "
                         "but current t=%llu — ignoring\n",
                         requester.c_str(),
                         static_cast<unsigned long long>(
                             m.expected_last_copy_t),
                         static_cast<unsigned long long>(current_t));
            return;
        }

        // Text / HTML / image reply (inline single message).
        if (!text_snapshot.empty()) {
            control::ClipboardUpdateMessage reply;
            reply.source_machine = local_machine_id_;
            reply.text           = text_snapshot.text;
            if (policy.allow_rich) {
                reply.html        = text_snapshot.html;
                reply.image_mime  = text_snapshot.image_mime;
                reply.image_bytes = text_snapshot.image_bytes;
            }
            const auto body = control::encode_clipboard(reply);
            std::fprintf(stderr,
                         "clipboard: fetch reply text=%zu html=%zu "
                         "image=%zu → %s\n",
                         reply.text.size(), reply.html.size(),
                         reply.image_bytes.size(),
                         requester.c_str());
            control_channel_->send(requester,
                                    control::MessageType::ClipboardUpdate,
                                    body.data(), body.size());
        }

        // File reply — kick off a sender targeting only the
        // requester. The existing FileTransferStart/Chunk/End
        // sequence is reused; the receiver's
        // @ref FileTransferReceiver materialises into
        // /tmp/unio-clipboard/<id>/ and writes the OS clipboard
        // on End.
        if (!files_snapshot.empty() && policy.allow_files) {
            std::uint64_t transfer_id;
            {
                static std::mt19937_64 rng(
                    std::random_device{}());
                static std::mutex      rng_m;
                std::lock_guard lk(rng_m);
                transfer_id = rng();
            }
            std::fprintf(stderr,
                         "file_xfer: fetch reply tx=%llu "
                         "(%zu files) → %s\n",
                         static_cast<unsigned long long>(transfer_id),
                         files_snapshot.files.size(),
                         requester.c_str());
            auto sender = std::make_unique<FileTransferSender>(
                data_channel_.get(),
                transfer_id,
                std::vector<std::string>{requester},
                files_snapshot.files,
                files_snapshot.selection_roots,
                local_machine_id_,
                [this](std::uint64_t id) {
                    // Linger before erase so the overlay's
                    // 10 Hz refresh catches short transfers and
                    // the user sees a brief "done" / "cancelled"
                    // state instead of the row vanishing.
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1200));
                    std::lock_guard lk(file_senders_m_);
                    file_senders_.erase(id);
                });
            std::lock_guard lk(file_senders_m_);
            file_senders_[transfer_id] = std::move(sender);
        }
    }

    // ── Ctrl+V intercept ─────────────────────────────────────
    //
    // The user pressing Ctrl+V on the active peer is the genuine
    // "paste now" trigger. We intercept the forwarded V keydown
    // (which always arrives as a @ref KeyEventMessage from the
    // dormant peer's keyboard, since the dormant peer holds the
    // physical keyboard and forwards every key while the cursor
    // is here), kick off a fetch from whoever last broadcast a
    // @ref ClipboardLatestMessage, then defer the V injection
    // until the fetch reply lands and the local clipboard is
    // populated. By the time we synthesise the V keystroke the
    // OS-side paste reads the freshly-arrived content.
    //
    // If the local clipboard is already up-to-date with the
    // freshest known announce — e.g. the user already pasted
    // the same content once — we don't intercept; the keystroke
    // flows through unchanged.

    /// @brief HID Usage IDs we care about for the Ctrl+V
    /// intercept. Values from the Keyboard/Keypad page (0x07).
    static constexpr std::uint32_t kHidV         = 0x19;
    static constexpr std::uint32_t kHidLeftCtrl  = 0xE0;
    static constexpr std::uint32_t kHidRightCtrl = 0xE4;
    static constexpr std::uint32_t kHidEscape    = 0x29;

    /// @brief Cancel one specific transfer by id. Called from
    /// the overlay's per-row cancel button. Handles both
    /// directions: an outbound entry (we're the source, the
    /// user clicked cancel on our "Sending →" row) flips the
    /// matching @ref FileTransferSender's cancel flag; an
    /// inbound entry (we're the destination, user clicked
    /// cancel on a "Receiving from" row) sends a
    /// @ref FileTransferCancel back to the source and tears
    /// down the receiver's local state.
    void cancel_transfer(std::uint64_t transfer_id) {
        std::fprintf(stderr,
                     "file_xfer: user cancel tx=%llu\n",
                     static_cast<unsigned long long>(transfer_id));
        // Outbound first.
        {
            std::lock_guard lk(file_senders_m_);
            auto it = file_senders_.find(transfer_id);
            if (it != file_senders_.end() && it->second) {
                it->second->cancel("user cancelled");
                return;
            }
        }
        // Inbound — find the source from the receiver's
        // active set, signal the source, drop our state.
        if (!file_transfer_receiver_ || !control_channel_) return;
        const auto active =
            file_transfer_receiver_->progress_snapshot();
        for (const auto& a : active) {
            if (a.transfer_id != transfer_id) continue;
            control::FileTransferCancelMessage cm;
            cm.transfer_id = transfer_id;
            cm.reason      = "user cancelled";
            const auto body = control::encode_file_cancel(cm);
            // File-transfer cancel rides the data channel
            // (same channel as the chunks it's stopping).
            if (data_channel_) {
                data_channel_->send(
                    a.source_machine,
                    control::MessageType::FileTransferCancel,
                    body.data(), body.size());
            }
            file_transfer_receiver_->on_cancel(a.source_machine, cm);
            // Cancel implicitly kills any deferred Ctrl+V
            // waiting on this transfer's bytes — otherwise
            // the V would never be released.
            paste_pending_ = false;
            v_swallowed_   = false;
            paste_ctrl_was_held_for_inject_ = false;
            return;
        }
    }

    /// @brief User pressed Escape while a Ctrl+V was in flight —
    /// cancel any inbound file transfers tied to the pending
    /// paste, drop the swallowed V state, and let the user move
    /// on. The cancel message reaches the source's matching
    /// sender via the bidirectional @ref FileTransferCancel
    /// handler below; senders see @c cancel_flag_ flip and
    /// stop streaming on the next chunk boundary.
    void abort_pending_paste() {
        if (!paste_pending_) return;
        std::fprintf(stderr,
                     "clipboard: aborting pending paste (Esc)\n");
        paste_pending_                  = false;
        paste_ctrl_was_held_for_inject_ = false;
        if (!file_transfer_receiver_) return;
        const auto active =
            file_transfer_receiver_->progress_snapshot();
        for (const auto& a : active) {
            control::FileTransferCancelMessage cm;
            cm.transfer_id = a.transfer_id;
            cm.reason      = "user-cancelled paste";
            const auto body = control::encode_file_cancel(cm);
            if (data_channel_) {
                data_channel_->send(
                    a.source_machine,
                    control::MessageType::FileTransferCancel,
                    body.data(), body.size());
            }
            // Tear down our receiver-side state without
            // waiting for the source to ack.
            file_transfer_receiver_->on_cancel(a.source_machine, cm);
        }
    }

    /// @brief Examine an inbound forwarded key event. Returns
    /// @c true when the event was swallowed (caller must not
    /// inject it); @c false otherwise.
    bool intercept_ctrl_v(std::uint32_t scancode, bool pressed) {
        // Escape during a pending paste is the user's "abort"
        // signal — cancel + drop the swallowed V so the OS
        // never sees the half-pasted keystroke.
        if (scancode == kHidEscape && pressed && paste_pending_) {
            abort_pending_paste();
            v_swallowed_ = false;
            return true;
        }
        // Track Ctrl held state across both modifier keys.
        if (scancode == kHidLeftCtrl || scancode == kHidRightCtrl) {
            if (pressed) ++ctrl_held_count_;
            else if (ctrl_held_count_ > 0) --ctrl_held_count_;
            return false;
        }
        if (scancode != kHidV) return false;

        if (pressed) {
            const bool ctrl_held = ctrl_held_count_ > 0;
            if (!ctrl_held) return false;
            if (!cursor_router_
                || !cursor_router_->is_local_active()) {
                return false;
            }
            // A previous Ctrl+V is still in flight — swallow
            // this one too so the OS can't paste stale content
            // from a fall-through. The deferred injection in
            // @ref release_pending_paste fires exactly once
            // when the fetch reply lands.
            if (paste_pending_) {
                v_swallowed_ = true;
                return true;
            }
            // If our local clipboard is already on the freshest
            // known (source, t) tuple, no fetch needed — let
            // the V down flow through and paste from whatever's
            // already on the local clipboard.
            //
            // Exception: file content uses "cut" / MOVE drop-
            // effect semantics, so the staged files are
            // consumed by the first paste. A second Ctrl+V on
            // the same (source, t) tuple needs a fresh fetch
            // to re-stage; otherwise the OS would try to paste
            // from a path that no longer exists. Text and image
            // content stay on the clipboard and can be pasted
            // repeatedly without refetching.
            std::string source;
            std::uint64_t t = 0;
            bool          has_files = false;
            {
                std::lock_guard lk(remote_clip_m_);
                if (latest_remote_source_.empty()) return false;
                has_files = (latest_remote_flags_ & 0x08) != 0;
                if (!has_files
                    && latest_remote_source_ == last_fetched_source_
                    && latest_remote_t_   == last_fetched_t_) {
                    return false;
                }
                source = latest_remote_source_;
                t      = latest_remote_t_;
            }
            std::fprintf(stderr,
                         "clipboard: Ctrl+V intercept — "
                         "fetch %s t=%llu, deferring V\n",
                         source.c_str(),
                         static_cast<unsigned long long>(t));
            v_swallowed_       = true;
            paste_pending_     = true;
            paste_ctrl_was_held_for_inject_ = (ctrl_held_count_ > 0);
            maybe_fetch_clipboard("ctrl-v");
            return true;
        }

        // V keyup: drop if the corresponding keydown was
        // swallowed. The synthesised release fires from
        // @ref release_pending_paste so the OS sees a fully-
        // formed key sequence.
        if (v_swallowed_) {
            v_swallowed_ = false;
            return true;
        }
        return false;
    }

    /// @brief Synthesise the deferred Ctrl+V keystroke once the
    /// local clipboard has been updated by an inbound fetch
    /// reply. Idempotent — safe to call from both the text/
    /// image reply path (@ref handle_clipboard_inbound) and the
    /// file reply path (@ref FileTransferReceiver's @c
    /// on_published callback).
    void release_pending_paste() {
        if (!paste_pending_ || !input_backend_) return;
        paste_pending_ = false;
        std::fprintf(stderr,
                     "clipboard: releasing deferred V keystroke\n");
        // Re-assert Ctrl if the user released it during the
        // fetch — otherwise V alone would just type 'v'.
        const bool need_ctrl =
            (ctrl_held_count_ == 0)
            && paste_ctrl_was_held_for_inject_;
        if (need_ctrl) {
            input_backend_->inject_key(kHidLeftCtrl, true);
        }
        input_backend_->inject_key(kHidV, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        input_backend_->inject_key(kHidV, false);
        if (need_ctrl) {
            input_backend_->inject_key(kHidLeftCtrl, false);
        }
    }

    /// @brief Inbound clipboard frame from a peer. Receiving is
    /// unconditional within the workspace — a PC that didn't
    /// opt in to broadcasting its own clipboard still pastes
    /// content from peers that did. Defensive gates re-apply
    /// here in case the sender's policy diverged from ours:
    ///   * sender must share our active workspace.
    ///   * if our workspace's @c clipboard_rich is off, drop
    ///     the rich + image fields and apply only text.
    ///   * if text exceeds our @c clipboard_max, drop the
    ///     text field (not the whole frame — rich + image
    ///     still apply on receive).
    void handle_clipboard_inbound(const std::string& peer,
                                    const control::ClipboardUpdateMessage& m) {
        if (!clipboard_backend_) return;
        const auto policy = current_clipboard_policy();
        std::fprintf(stderr,
                     "clipboard: inbound from %s "
                     "(text=%zu, html=%zu, image=%zu mime=%s) "
                     "allow_rich=%d\n",
                     peer.c_str(),
                     m.text.size(), m.html.size(),
                     m.image_bytes.size(),
                     m.image_mime.empty() ? "-"
                                            : m.image_mime.c_str(),
                     policy.allow_rich ? 1 : 0);
        if (policy.ws_members.count(peer) == 0) {
            return;
        }

        ClipboardData data;
        if (policy.max_text_bytes == 0
            || m.text.size() <= policy.max_text_bytes) {
            data.text = m.text;
        } else {
            std::fprintf(stderr,
                         "clipboard: inbound text from %s exceeds "
                         "limit (%zu > %zu) — text dropped\n",
                         peer.c_str(), m.text.size(),
                         policy.max_text_bytes);
        }
        if (policy.allow_rich) {
            data.html        = m.html;
            data.image_mime  = m.image_mime;
            data.image_bytes = m.image_bytes;
        }
        if (data.empty()) return;

        if (clipboard_monitor_) {
            // Mark BEFORE the write so the next poll that
            // reads this exact payload doesn't re-broadcast.
            clipboard_monitor_->note_inbound(data);
        }
        clipboard_backend_->set_clipboard(data);
        release_pending_paste();
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
                // Every workspace member contributes its
                // monitors to the routing layout, including
                // peers with Input unchecked: a checked peer
                // is allowed to push the global cursor onto
                // an unchecked peer (and continue driving it
                // there from the checked peer's mouse +
                // keyboard). What "Input unchecked" forbids
                // is local-mouse-initiated handoffs from the
                // unchecked peer outward — that gate lives
                // inside the cursor router on the local
                // peer's @c is_cursor_member_ flag.
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
    /// fires the auth-state-changed transition once, then runs a
    /// periodic local-probe loop so monitor hot-plugs (display
    /// connected / disconnected mid-session) propagate into the
    /// mesh caps + cursor router without restarting the app.
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

        // Periodic local-probe loop. Cheap on both platforms
        // (RandR query on Linux, EnumDisplayMonitors on Win)
        // and kept on the worker thread so we don't introduce
        // a separate rescan thread for monitor hot-plug.
        constexpr auto kProbeInterval = std::chrono::seconds(2);
        std::vector<Display> last_displays;
        if (local_probe_) last_displays = local_probe_->probe().displays;
        auto next_probe =
            std::chrono::steady_clock::now() + kProbeInterval;
        while (!stop_flag_.load()) {
            {
                std::unique_lock lk(cv_m_);
                cv_.wait_until(lk, next_probe,
                                [&]{ return stop_flag_.load(); });
            }
            if (stop_flag_.load()) return;
            next_probe += kProbeInterval;
            if (!local_probe_) continue;
            auto caps = local_probe_->probe();
            if (caps.displays == last_displays) continue;
            std::fprintf(stderr,
                         "probe: local displays changed "
                         "(%zu → %zu)\n",
                         last_displays.size(),
                         caps.displays.size());
            last_displays = caps.displays;
            // publish_local_caps overrides machine_id +
            // display_name + per-display machine_id with the
            // local hostname so identity routes hook up
            // consistently — replicate that here.
            caps.machine_id   = local_machine_id_;
            caps.display_name = local_display_name_;
            for (auto& d : caps.displays) {
                d.machine_id = local_machine_id_;
            }
            if (mesh_) mesh_->put_caps(caps);
            // Force an immediate announce so peers see the new
            // monitor sub-second instead of waiting for the
            // next 2s discovery tick.
            if (discovery_) discovery_->trigger_announce_now();
            refresh_cursor_router_state();
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
    /// @brief Sibling data-channel listen port — second TCP
    /// socket dedicated to file-transfer frames so chunks
    /// don't compete with cursor / keys for the per-link
    /// send mutex on @ref control_channel_.
    std::uint16_t                                    data_port_    = 0;
    std::unique_ptr<control::IControlChannel>        control_channel_;
    std::unique_ptr<control::IControlChannel>        data_channel_;
    std::unique_ptr<input::IInputBackend>            input_backend_;
    /// @brief Plain-text clipboard backend (X11 native selection
    /// protocol on Linux, Win32 OpenClipboard on Windows). The
    /// monitor below polls it.
    std::unique_ptr<IClipboardBackend>               clipboard_backend_;
    /// @brief Background polling thread that detects local
    /// clipboard changes and fires the orchestrator's outbound
    /// gate. Built in wire_clipboard_monitor once the backend
    /// is live.
    std::unique_ptr<ClipboardMonitor>                clipboard_monitor_;
    /// @brief Local clipboard cache + monotonic copy counter.
    /// Updated whenever the monitor sees the user perform a
    /// fresh local Ctrl+C. Read on inbound
    /// @ref ClipboardFetchMessage to build the reply. Held
    /// behind @ref local_clip_m_ so the monitor thread (writer)
    /// doesn't race the control reader thread (reader).
    mutable std::mutex                               local_clip_m_;
    ClipboardData                                    local_text_;
    ClipboardFiles                                   local_files_;
    std::uint64_t                                    local_last_copy_t_ = 0;

    /// @brief Most recently received @ref ClipboardLatestMessage
    /// from another peer. We track only the latest because
    /// the user's "last Ctrl+C wins" rule maps directly to
    /// receive-order — any new announcement supersedes the
    /// previous, regardless of which peer it's from.
    mutable std::mutex                               remote_clip_m_;
    std::string                                      latest_remote_source_;
    std::uint64_t                                    latest_remote_t_     = 0;
    std::uint8_t                                     latest_remote_flags_ = 0;
    /// @brief De-duplication: the (source, t) we most recently
    /// pulled. Re-firing @ref maybe_fetch_clipboard for the
    /// same tuple is a no-op so a repeat Ctrl+V on the same
    /// content doesn't refetch.
    std::string                                      last_fetched_source_;
    std::uint64_t                                    last_fetched_t_      = 0;

    /// @brief Ctrl+V intercept state. Touched only from the
    /// control reader thread (KeyEvent dispatch + clipboard /
    /// file inbound), so no mutex needed.
    int                                              ctrl_held_count_   = 0;
    bool                                             v_swallowed_       = false;
    bool                                             paste_pending_     = false;
    bool                                             paste_ctrl_was_held_for_inject_ = false;

    /// @brief Active file-transfer senders keyed by
    /// transfer_id. Each sender owns its background thread;
    /// finished senders self-erase via their on_finished
    /// callback. Held under @ref file_senders_m_ so the
    /// monitor's spawning and the senders' completion
    /// callbacks can mutate it concurrently.
    mutable std::mutex                               file_senders_m_;
    std::unordered_map<std::uint64_t,
                       std::unique_ptr<FileTransferSender>> file_senders_;
    /// @brief Inbound file-transfer assembler. Each transfer
    /// materialises into a per-id subdir under
    /// std::filesystem::temp_directory_path()/unio-clipboard.
    std::unique_ptr<FileTransferReceiver>            file_transfer_receiver_;
    /// @brief Floating progress overlay (X11/Win32). Owns its
    /// own native window + refresh thread; the orchestrator
    /// only wires the progress fetcher and lifecycle.
    std::unique_ptr<platform::ITransferOverlay>      transfer_overlay_;
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

    /// @brief Polled cursor position the last time the raw-input
    /// on_motion callback ran. Each on_motion forwards
    /// (current_polled - last_polled) so the OS-level cursor
    /// clamp at the source monitor's edge naturally throttles
    /// sustained motion past the edge — receiver doesn't get
    /// flooded with deltas when the user keeps pushing after
    /// a cross.
    /// @brief Owns the dormant-mode polled-cursor delta
    /// forwarder. Built in wire_raw_input_capture once the
    /// input_backend + cursor_router are live; null on a peer
    /// that doesn't have a usable input backend.
    std::unique_ptr<input::PolledMotionForwarder>    motion_forwarder_;

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
