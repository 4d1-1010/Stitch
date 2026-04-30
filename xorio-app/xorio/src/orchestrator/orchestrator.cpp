/// @file orchestrator.cpp
/// @brief Façade construction + lifecycle + control/data channel
/// dispatcher + worker thread. The concrete @ref FacadeOrchestrator
/// class lives in @c facade.hpp; per-concern method bodies live in
/// dedicated TUs:
///   * @c clipboard/coordinator.cpp — clipboard / file-transfer.
///   * @c cursor_handoff.cpp        — cursor router glue.

#include "orchestrator/facade.hpp"

#include "mock/factories.hpp"
#include "workspace/factory.hpp"

#include <cstdio>

#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <signal.h>
#  include <sys/prctl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace xorio::orchestrator {

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

}  // namespace

namespace detail {

FacadeOrchestrator::FacadeOrchestrator(OrchestratorCallbacks cb)
    : callbacks_(std::move(cb)),
      start_time_(std::chrono::steady_clock::now()),
      local_machine_id_(local_hostname()),
      local_display_name_(local_machine_id_),
      local_probe_(make_local_probe()),
      mesh_(make_mock_mesh_crdt(local_machine_id_)),
      // Workspaces sub-module is constructed BEFORE discovery so
      // the discovery config's workspaces_provider can capture
      // &*workspaces_ safely. Member-init order matches
      // declaration order in C++; declaration in this class
      // already places workspaces_ above discovery_-adjacent
      // members in the init list ordering below — but the C++
      // language requires member-init order = declaration order,
      // so we declare workspaces_ above discovery_ in the data-
      // member section in facade.hpp.
      workspaces_(make_workspace_manager()),
      // Real peer-to-peer TCP control channel. The helper starts
      // it and writes the bound port to control_port_ BEFORE the
      // discovery config below reads control_port_ (init order
      // matches declaration order: control_port_, then
      // control_channel_, then discovery_).
      control_channel_(start_control_channel(local_machine_id_,
                                              control_port_)),
      // Sibling channel exclusively for file-transfer frames.
      // Identical implementation, distinct listen port, so chunks
      // don't share a send mutex with cursor / key / clipboard-
      // control frames. Bound port written into data_port_ before
      // the discovery config below reads it.
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
              // overlay through the same callback the local click
              // uses.
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
    // peers see the change sub-second instead of waiting for the
    // next 2s tick. Both local mutations + remote merges funnel
    // through the on_changed callback; the broadcast is
    // idempotent (LWW on the receive side ignores no-op updates),
    // so re-broadcasting after a merge is harmless.
    workspaces_->set_on_changed(
        [this](const std::string& /*workspace_id*/) {
            if (discovery_) discovery_->trigger_announce_now();
            refresh_cursor_router_state();
        });
    refresh_cursor_router_state();
    worker_ = std::thread(&FacadeOrchestrator::run, this);
}

FacadeOrchestrator::~FacadeOrchestrator() {
    stop_flag_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (cursor_poller_)     cursor_poller_->stop();
    if (clipboard_monitor_) clipboard_monitor_->stop();
    // Release the sleep inhibitor before any other teardown.
    // On Linux this kills the child systemd-inhibit so it
    // doesn't outlive us holding a stale lock; on Windows
    // SetThreadExecutionState clears the per-thread flag
    // (the thread is about to die anyway, but explicit is
    // better).
#if defined(_WIN32)
    if (sleep_inhibited_) {
        ::SetThreadExecutionState(ES_CONTINUOUS);
        sleep_inhibited_ = false;
    }
#else
    if (sleep_inhibitor_pid_ != 0) {
        ::kill(sleep_inhibitor_pid_, SIGTERM);
        ::waitpid(sleep_inhibitor_pid_, nullptr, 0);
        sleep_inhibitor_pid_ = 0;
    }
#endif
    // Tear the overlay down before the senders/receiver so the
    // refresh thread can't race a freed @ref file_senders_ map on
    // its way out.
    if (transfer_overlay_)  transfer_overlay_->stop();
    discovery_->stop();
    if (control_channel_)   control_channel_->stop();
    if (data_channel_)      data_channel_->stop();
    if (input_backend_)     input_backend_->close();
    if (clipboard_backend_) clipboard_backend_->close();
}

bool FacadeOrchestrator::try_authorize(const std::string& key) {
    if (key != kAccessGateKey) return false;
    access_authorized_.store(true, std::memory_order_release);
    return true;
}

void FacadeOrchestrator::request_identify() {
    identify_counter_.fetch_add(1, std::memory_order_acq_rel);
    // Force an immediate announce so peers see the bump without
    // the up-to-2-second wait of the regular cadence.
    if (discovery_) discovery_->trigger_announce_now();
    if (callbacks_.on_identify_request) {
        callbacks_.on_identify_request();
    }
}

void FacadeOrchestrator::wire_sub_modules() {
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

void FacadeOrchestrator::publish_local_caps() {
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

void FacadeOrchestrator::wire_control_channel_callbacks() {
    if (!control_channel_) return;
    control_channel_->set_callbacks(
        [this](const std::string& peer, const control::InboundFrame& f) {
            switch (f.type) {
                case control::MessageType::Heartbeat:
                    break;
                case control::MessageType::MouseMoveAbs: {
                    // Phase C: cursor stays local on the active
                    // peer; we never receive raw move events
                    // anymore, only Handoffs. Logged for debug if
                    // a peer still uses Phase B.
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
                        // Surface the cursor + pre-fetch any
                        // pending remote clipboard so a local
                        // Ctrl+V on this PC pastes the fresh bytes.
                        // Active-peer paste hits the OS clipboard
                        // directly (not through intercept_ctrl_v,
                        // which only sees forwarded keys) so the
                        // bytes must be there before the user
                        // types V. The dedupe inside
                        // maybe_fetch_clipboard makes a flyby
                        // cheap: if (source, t) matches what we
                        // last pulled, no fetch fires.
                        sync_cursor_visibility_locked();
                        maybe_fetch_clipboard("handoff-arrival");
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
                    // Dormant peer is forwarding the user's mouse
                    // motion to us. We hand it to the cursor
                    // router rather than injecting directly: the
                    // router updates an internal tracked
                    // workspace position (used for edge
                    // detection) and warps the OS cursor to
                    // follow. Tracking separately from the OS
                    // cursor is what keeps touchpad-driver
                    // phantom corrections from bouncing the
                    // cursor back to the source — Barrier's
                    // client-side model.
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
                        // Track the user's modifier state from the
                        // forwarded keystream BEFORE running the
                        // Ctrl+V intercept (which only reads the
                        // counter) and BEFORE injecting locally.
                        // The user's keyboard lives on the dormant
                        // peer that's forwarding to us, so this is
                        // the only place we see their modifier
                        // toggles when we're the active receiver.
                        track_modifier_key(m->scancode, m->pressed);
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
                // File-transfer frames now arrive on the sibling
                // data channel so chunks don't queue cursor /
                // keyboard messages behind the per-link send
                // mutex on this channel. See
                // @ref wire_data_channel_callbacks.
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
            update_sleep_inhibitor_state();
            refresh_cursor_router_state();
        },
        [this](const std::string& peer) {
            std::fprintf(stderr, "control: disconnected → %s\n",
                         peer.c_str());
            {
                std::lock_guard lk(connected_peers_m_);
                connected_peers_.erase(peer);
            }
            // Reclaim active state if the disconnected peer was
            // the one holding our cursor (we were dormant and
            // forwarding to it) or visiting us. Otherwise the
            // input grab + hidden cursor would strand us with no
            // way to drive any PC. refresh_cursor_router_state
            // below picks up the new active flag and ungrabs via
            // sync_cursor_visibility_locked.
            if (cursor_router_) cursor_router_->on_peer_lost(peer);
            update_cursor_poller_state();
            update_sleep_inhibitor_state();
            refresh_cursor_router_state();
        });
}

void FacadeOrchestrator::wire_data_channel_callbacks() {
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

std::vector<net::AnnounceDisplay>
FacadeOrchestrator::wire_displays_for_announce() const {
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

std::vector<net::AnnounceWorkspace>
FacadeOrchestrator::wire_workspaces_for_announce() const {
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

void FacadeOrchestrator::run() {
    wait_until(std::chrono::milliseconds(1000));
    if (stop_flag_.load()) return;

    discovery_->start(
        [this](const DiscoveryAnnouncement& a) {
            peer_events_.handle_peer_observed(a);
            // Re-derive the cursor router's monitor list every
            // time we see an announce. The workspace-change
            // callback only fires when LWW actually mutates
            // state, so a stable workspace whose member peers'
            // display caps just arrived wouldn't otherwise
            // refresh the router. Without this refresh, Diana's
            // adjacency search has no adi-pc monitors to find as
            // neighbours and a return-edge handoff silently
            // fails.
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

    // Periodic local-probe loop. Cheap on both platforms (RandR
    // query on Linux, EnumDisplayMonitors on Win) and kept on the
    // worker thread so we don't introduce a separate rescan
    // thread for monitor hot-plug.
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
        // publish_local_caps overrides machine_id + display_name
        // + per-display machine_id with the local hostname so
        // identity routes hook up consistently — replicate that
        // here.
        caps.machine_id   = local_machine_id_;
        caps.display_name = local_display_name_;
        for (auto& d : caps.displays) {
            d.machine_id = local_machine_id_;
        }
        if (mesh_) mesh_->put_caps(caps);
        // Force an immediate announce so peers see the new
        // monitor sub-second instead of waiting for the next 2s
        // discovery tick.
        if (discovery_) discovery_->trigger_announce_now();
        refresh_cursor_router_state();
    }
}

void FacadeOrchestrator::update_sleep_inhibitor_state() {
    bool any_peer = false;
    {
        std::lock_guard lk(connected_peers_m_);
        any_peer = !connected_peers_.empty();
    }
#if defined(_WIN32)
    // SetThreadExecutionState is per-thread state. The combined
    // ES_CONTINUOUS|ES_SYSTEM_REQUIRED|ES_DISPLAY_REQUIRED
    // request keeps the system out of sleep AND keeps the
    // display from blanking — display blank in many Win configs
    // immediately triggers the lock screen, which blocks all
    // forwarded keystrokes via Secure Desktop.
    if (any_peer && !sleep_inhibited_) {
        ::SetThreadExecutionState(
            ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
        sleep_inhibited_ = true;
        std::fprintf(stderr,
                     "sleep: inhibitor engaged "
                     "(peer connected — system + display kept awake)\n");
    } else if (!any_peer && sleep_inhibited_) {
        ::SetThreadExecutionState(ES_CONTINUOUS);
        sleep_inhibited_ = false;
        std::fprintf(stderr, "sleep: inhibitor released\n");
    }
#else
    // systemd-inhibit holds a session-bus inhibition lock for as
    // long as the spawned process lives. We exec it with `sleep
    // infinity` as the held command so it runs forever; killing
    // the process releases the lock. This avoids linking
    // libdbus / sd-bus (per the project's minimise-deps rule).
    // PR_SET_PDEATHSIG ensures the child gets SIGTERM if our
    // process is hard-killed (SIGKILL), so we don't strand a
    // stale inhibitor.
    if (any_peer && sleep_inhibitor_pid_ == 0) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGTERM);
            ::execlp("systemd-inhibit",
                     "systemd-inhibit",
                     "--what=idle:sleep:handle-lid-switch",
                     "--who=xorio",
                     "--why=cross-PC mesh in use",
                     "--mode=block",
                     "sleep", "infinity",
                     static_cast<char*>(nullptr));
            ::_exit(127);  // exec failed
        } else if (pid > 0) {
            sleep_inhibitor_pid_ = pid;
            std::fprintf(stderr,
                         "sleep: inhibitor engaged "
                         "(systemd-inhibit pid=%d)\n",
                         pid);
        } else {
            std::fprintf(stderr,
                         "sleep: fork() for systemd-inhibit failed\n");
        }
    } else if (!any_peer && sleep_inhibitor_pid_ != 0) {
        ::kill(sleep_inhibitor_pid_, SIGTERM);
        ::waitpid(sleep_inhibitor_pid_, nullptr, 0);
        sleep_inhibitor_pid_ = 0;
        std::fprintf(stderr, "sleep: inhibitor released\n");
    }
#endif
}

void FacadeOrchestrator::track_modifier_key(std::uint32_t scancode,
                                              bool pressed) {
    bool ctrl_or_shift_changed = false;
    if (scancode == kHidLeftCtrl || scancode == kHidRightCtrl) {
        if (pressed) ++ctrl_held_count_;
        else if (ctrl_held_count_ > 0) --ctrl_held_count_;
        ctrl_or_shift_changed = true;
    } else if (scancode == kHidLeftShift || scancode == kHidRightShift) {
        if (pressed) ++shift_held_count_;
        else if (shift_held_count_ > 0) --shift_held_count_;
        ctrl_or_shift_changed = true;
    } else if (scancode == kHidLeftAlt || scancode == kHidRightAlt) {
        if (pressed) ++alt_held_count_;
        else if (alt_held_count_ > 0) --alt_held_count_;
    } else if (scancode == kHidLeftWin || scancode == kHidRightWin) {
        if (pressed) ++win_held_count_;
        else if (win_held_count_ > 0) --win_held_count_;
    }
    // Push only Ctrl+Shift to the cursor router — that's the
    // pair the "Hold Ctrl+Shift to cross" gate cares about. Alt
    // and Win counters are read inline by the OS-hotkey forward
    // gate, no router involvement.
    if (ctrl_or_shift_changed && cursor_router_) {
        const bool held =
            ctrl_held_count_ > 0 && shift_held_count_ > 0;
        cursor_router_->set_modifier_held(held);
    }
}

void FacadeOrchestrator::wait_until(std::chrono::milliseconds delay) {
    std::unique_lock lk(cv_m_);
    cv_.wait_until(lk, start_time_ + delay,
                   [&] { return stop_flag_.load(); });
}

}  // namespace detail

std::unique_ptr<IOrchestrator>
make_mock(const OrchestratorCallbacks& callbacks) {
    return std::make_unique<detail::FacadeOrchestrator>(callbacks);
}

}  // namespace xorio::orchestrator
