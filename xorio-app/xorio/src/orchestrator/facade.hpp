/// @file facade.hpp
/// @brief Private declaration of @c FacadeOrchestrator — the
/// concrete @ref IOrchestrator implementation. Lives in src/
/// rather than include/ because no public consumer needs the
/// class shape; the only entry points are the @ref make_mock
/// factory in @c orchestrator.cpp and the per-concern method
/// bodies split across:
///   * @c orchestrator.cpp           — ctor / dtor / control + data
///                                     channel dispatcher / sub-module
///                                     composition / worker thread.
///   * @c clipboard/coordinator.cpp  — clipboard capture, announce,
///                                     fetch, paste-intercept, file-
///                                     transfer wiring.
///   * @c cursor_handoff.cpp         — cursor poller wiring, handoff
///                                     send, edge-pin warp, raw-input
///                                     forwarding, router refresh.
///
/// Trivial public-API forwards stay inline below — every override
/// is a 1-3 line delegation to a sub-module, so out-of-lining
/// them would split a single concern across two files for no
/// gain.
#pragma once

#include "orchestrator/orchestrator.hpp"

#include "orchestrator/clipboard/backend.hpp"
#include "orchestrator/clipboard/monitor.hpp"
#include "orchestrator/control/control_channel.hpp"
#include "orchestrator/control/protocol.hpp"
#include "orchestrator/control_connection.hpp"
#include "orchestrator/cursor_router.hpp"
#include "orchestrator/discovery.hpp"
#include "orchestrator/file_transfer/receiver.hpp"
#include "orchestrator/file_transfer/sender.hpp"
#include "orchestrator/input/cursor_poller.hpp"
#include "orchestrator/input/input_backend.hpp"
#include "orchestrator/input/polled_motion_forwarder.hpp"
#include "orchestrator/local_probe/local_probe.hpp"
#include "orchestrator/media_connection.hpp"
#include "orchestrator/mesh_crdt.hpp"
#include "orchestrator/net/lan_discovery.hpp"
#include "orchestrator/pairing_manager.hpp"
#include "orchestrator/path_selector.hpp"
#include "orchestrator/peer_events.hpp"
#include "orchestrator/session_scheduler.hpp"
#include "orchestrator/workspace.hpp"

#include "platform/transfer_overlay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xorio::orchestrator::detail {

class FacadeOrchestrator final : public IOrchestrator {
public:
    explicit FacadeOrchestrator(OrchestratorCallbacks cb);
    ~FacadeOrchestrator() override;

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

    bool try_authorize(const std::string& key) override;

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

    void request_identify() override;

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
        std::string id = workspaces_->create(name, members, input_members,
                                              clipboard_members);
        // Seed a default layout so the cursor router has non-
        // overlapping mesh-global coords from frame one. Without
        // this, the router falls back to each peer's raw OS-local
        // (every "first monitor" starts at (0, 0) on its own
        // desktop), the rects overlap, and adjacency is undefined
        // so cursor crossings near (0, 0) silently fail. Same
        // arrangement the Layout tab shows by default — peers
        // stacked side-by-side in alphabetical order with a small
        // gap. The user can re-arrange via the Layout tab any time.
        if (!id.empty()) {
            auto layout = build_default_layout(members);
            if (!layout.empty()) {
                workspaces_->set_layout(id, layout);
            }
        }
        return id;
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

    void set_workspace_master_lock(
        const std::string& workspace_id,
        bool enable,
        std::uint32_t unlock_after_h) override {
        workspaces_->set_master_lock(workspace_id, enable,
                                      unlock_after_h, local_machine_id_);
    }

    void set_workspace_layout(
        const std::string& workspace_id,
        const std::vector<DisplayLayoutEntry>& layout) override {
        workspaces_->set_layout(workspace_id, layout);
    }

    void delete_workspace(const std::string& workspace_id) override {
        workspaces_->destroy(workspace_id);
    }

    void leave_workspace(const std::string& workspace_id) override;

    bool is_alone_in_workspace(const std::string& workspace_id) const override;
    bool is_alone_prompt_answered(const std::string& workspace_id) const override;
    void mark_alone_prompt_answered(const std::string& workspace_id) override;

    void acquire_workspace_lock(const std::string& workspace_id) override {
        workspaces_->acquire_lock(workspace_id, local_machine_id_);
    }

    void release_workspace_lock(const std::string& workspace_id) override {
        workspaces_->release_lock(workspace_id, local_machine_id_);
    }

private:
    // ── Composition + dispatcher (orchestrator.cpp) ────────
    void wire_sub_modules();
    void publish_local_caps();
    void wire_control_channel_callbacks();
    void wire_data_channel_callbacks();
    std::vector<net::AnnounceDisplay>   wire_displays_for_announce() const;
    std::vector<net::AnnounceWorkspace> wire_workspaces_for_announce() const;
    void run();
    void wait_until(std::chrono::milliseconds delay);

    /// @brief Engage / release the OS-level sleep + display
    /// inhibitor based on @ref connected_peers_. While at least
    /// one peer is connected, prevent system sleep AND display
    /// blank — the lock screen that follows either would block
    /// keyboard forwarding (Windows Secure Desktop / Linux
    /// session locker) and force the user to walk to the
    /// machine. Released when the last peer drops.
    void update_sleep_inhibitor_state();

    /// @brief Build the default per-peer-column display layout
    /// for a freshly-created workspace. Walks each member's caps
    /// from the mesh CRDT, stacks peers alphabetically with a
    /// small horizontal gap so their mesh-global rects never
    /// overlap; same shape the Layout tab shows by default.
    /// Empty when @p members has no displays known yet (the next
    /// announce-driven cap refresh will be a no-op since the
    /// layout is empty — user can apply manually later).
    std::vector<DisplayLayoutEntry> build_default_layout(
        const std::unordered_set<std::string>& members) const;

    /// @brief Fill in the default layout for any workspace whose
    /// `layout` is empty and where caps for every member are
    /// available. Idempotent across calls (once a workspace is
    /// seeded, it lands in @ref auto_layout_attempted_ so we
    /// don't ping-pong it on every announce). Called whenever
    /// the workspace catalogue changes or peer caps arrive, so
    /// pre-existing empty layouts get auto-seeded the moment we
    /// have enough info.
    void ensure_workspace_layouts();

    /// @brief Drive the OS-level display arrangement from the
    /// active workspace's layout entries for the local PC. The
    /// Layout tab is the source of truth as long as xorio is
    /// running and a workspace with @>= 2 members is active —
    /// dragging a local monitor in any peer's Layout tab moves
    /// the actual monitor on this PC via XRandR / Win32. No-op
    /// when there's no active multi-member workspace, when the
    /// layout is empty, or when the OS arrangement already
    /// matches the desired placement.
    void apply_local_arrangement_if_needed();

    /// @brief Recompute the alone-online state for every workspace
    /// and emit `on_workspace_alone_changed` for any that
    /// transitioned. Called from both peer-event handlers and from
    /// the workspace-change callback so the state stays consistent
    /// regardless of which side moved (peer online toggle vs
    /// member set update).
    void refresh_alone_state();

    /// @brief Scan workspaces and clear any Lock / Master-Lock
    /// whose idle (now - version_ns) has exceeded its configured
    /// `*_unlock_after_h` threshold. Called from the worker
    /// thread's periodic loop. Idle is reset on every whole-row
    /// mutation (the LWW path that bumps version_ns), so user
    /// activity naturally restarts the timer.
    void check_workspace_auto_unlock();

    // ── Cursor handoff (cursor_handoff.cpp) ────────────────
    void wire_cursor_poller();
    void update_cursor_poller_state();
    void send_handoff(const std::string& target,
                      std::int32_t entry_x,
                      std::int32_t entry_y);
    void sync_cursor_visibility_locked();
    void send_mouse_rel(const std::string& target,
                        std::int32_t dx,
                        std::int32_t dy);
    void wire_raw_input_capture();
    void refresh_cursor_router_state();

    // ── Clipboard coordinator (clipboard/coordinator.cpp) ──
    /// @brief Per-workspace policy snapshot. Read on every
    /// inbound + outbound clipboard frame so a workspace
    /// edit takes effect without restarting.
    struct ClipboardPolicy {
        std::unordered_set<std::string> ws_members;
        bool                            outbound_allowed = false;
        bool                            allow_rich       = false;
        bool                            allow_files      = false;
        std::size_t                     max_text_bytes   = 0;
    };
    static std::size_t clipboard_max_bytes(ClipboardLimit lim);
    ClipboardPolicy current_clipboard_policy() const;
    std::string peer_display_name_for(const std::string& mid) const;
    static std::string summarize_roots(
        const std::vector<std::string>& roots);

    void wire_clipboard_monitor();
    void wire_transfer_overlay();
    void wire_file_transfer_receiver();
    void capture_local_files(const ClipboardFiles& files);
    void capture_local_clipboard(const ClipboardData& data);
    void announce_local_clipboard_locked(bool has_text,
                                          bool has_html,
                                          bool has_image,
                                          bool has_files);
    void handle_clipboard_latest_inbound(
        const std::string& peer,
        const control::ClipboardLatestMessage& m);
    void maybe_fetch_clipboard(const char* trigger);
    void handle_clipboard_fetch_inbound(
        const std::string& requester,
        const control::ClipboardFetchMessage& m);
    void cancel_transfer(std::uint64_t transfer_id);
    void abort_pending_paste();
    bool intercept_ctrl_v(std::uint32_t scancode, bool pressed);
    void release_pending_paste();
    void handle_clipboard_inbound(
        const std::string& peer,
        const control::ClipboardUpdateMessage& m);

    /// @brief HID Usage IDs (Keyboard/Keypad page 0x07) the
    /// Ctrl+V intercept + workspace modifier/hotkey gates watch
    /// for. Alt + Win + Tab/Space/F4/Delete are tracked so the
    /// "Block OS hotkeys from forwarding" gate can recognise
    /// Win+anything, Alt+Tab/Esc/F4/Space, and Ctrl+Alt+Del.
    static constexpr std::uint32_t kHidV          = 0x19;
    static constexpr std::uint32_t kHidTab        = 0x2B;
    static constexpr std::uint32_t kHidSpace      = 0x2C;
    static constexpr std::uint32_t kHidF4         = 0x3D;
    static constexpr std::uint32_t kHidDelete     = 0x4C;
    static constexpr std::uint32_t kHidLeftCtrl   = 0xE0;
    static constexpr std::uint32_t kHidLeftShift  = 0xE1;
    static constexpr std::uint32_t kHidLeftAlt    = 0xE2;
    static constexpr std::uint32_t kHidLeftWin    = 0xE3;
    static constexpr std::uint32_t kHidRightCtrl  = 0xE4;
    static constexpr std::uint32_t kHidRightShift = 0xE5;
    static constexpr std::uint32_t kHidRightAlt   = 0xE6;
    static constexpr std::uint32_t kHidRightWin   = 0xE7;
    static constexpr std::uint32_t kHidEscape     = 0x29;

    /// @brief Update Ctrl/Shift/Alt/Win held counters when
    /// @p scancode is a modifier key, then push the combined
    /// Ctrl+Shift held state into @ref cursor_router_. Called
    /// from BOTH local raw key events (cbs.on_key in
    /// wire_raw_input_capture) and forwarded inbound keys
    /// (control-channel KeyEvent dispatcher), so the gates work
    /// regardless of which side the user's keyboard is at.
    void track_modifier_key(std::uint32_t scancode, bool pressed);

    /// @brief Recognise OS-level hotkey combinations the
    /// workspace's "Block OS hotkeys from forwarding" toggle
    /// should suppress. Pure function of scancode + the live
    /// Ctrl/Alt/Win held state — Ctrl+Shift+anything is
    /// intentionally allowed through (cross modifier).
    static bool is_blocked_os_hotkey(std::uint32_t scancode,
                                      bool ctrl, bool alt, bool win);

    // ── State ──────────────────────────────────────────────
    OrchestratorCallbacks                            callbacks_;
    std::chrono::steady_clock::time_point            start_time_;

    std::string                                      local_machine_id_;
    std::string                                      local_display_name_;

    std::unique_ptr<ILocalProbeAdapter>              local_probe_;
    std::unique_ptr<IMeshCrdt>                       mesh_;
    /// @brief Workspaces sub-module — constructed before
    /// discovery so the discovery config's workspaces_provider
    /// can capture &*workspaces_ safely (member-init order =
    /// declaration order in C++).
    std::unique_ptr<IWorkspaceManager>               workspaces_;
    /// @brief Control-channel listen port (set by
    /// start_control_channel during init). Declared before
    /// control_channel_ so its address is bindable inside the
    /// helper call; declared before discovery_ so the LAN
    /// announce config reads its post-helper value, not the
    /// pre-init zero.
    std::uint16_t                                    control_port_ = 0;
    /// @brief Sibling data-channel listen port — second TCP
    /// socket dedicated to file-transfer frames so chunks
    /// don't compete with cursor / keys for the per-link
    /// send mutex on @ref control_channel_.
    std::uint16_t                                    data_port_    = 0;
    std::unique_ptr<control::IControlChannel>        control_channel_;
    std::unique_ptr<control::IControlChannel>        data_channel_;
    std::unique_ptr<input::IInputBackend>            input_backend_;
    std::unique_ptr<IClipboardBackend>               clipboard_backend_;
    std::unique_ptr<ClipboardMonitor>                clipboard_monitor_;
    /// @brief Local clipboard cache + monotonic copy counter.
    /// Held behind @ref local_clip_m_ so the monitor thread
    /// (writer) doesn't race the control reader thread (reader).
    mutable std::mutex                               local_clip_m_;
    ClipboardData                                    local_text_;
    ClipboardFiles                                   local_files_;
    std::uint64_t                                    local_last_copy_t_ = 0;

    /// @brief Most recently received @ref ClipboardLatestMessage
    /// from another peer.
    mutable std::mutex                               remote_clip_m_;
    std::string                                      latest_remote_source_;
    std::uint64_t                                    latest_remote_t_     = 0;
    std::uint8_t                                     latest_remote_flags_ = 0;
    std::string                                      last_fetched_source_;
    std::uint64_t                                    last_fetched_t_      = 0;

    /// @brief Ctrl+V intercept state. Touched only from the
    /// control reader thread (KeyEvent dispatch + clipboard /
    /// file inbound), so no mutex needed.
    int                                              ctrl_held_count_   = 0;
    int                                              shift_held_count_  = 0;
    int                                              alt_held_count_    = 0;
    int                                              win_held_count_    = 0;
    /// @brief Workspace's "Block OS hotkeys from forwarding"
    /// toggle. Read from the active workspace by
    /// @ref refresh_cursor_router_state and consumed by the local
    /// raw-key forward path (cbs.on_key in
    /// @ref wire_raw_input_capture). Only the forward path
    /// gates on this — receivers don't re-check, since the
    /// workspace value is propagated via LWW so both ends agree.
    bool                                             block_hotkeys_     = false;

    /// @brief Sleep / display inhibitor state. Engaged whenever
    /// @ref connected_peers_ is non-empty. Platform-specific
    /// representation: Windows uses per-thread
    /// SetThreadExecutionState (no handle to track, just a bool);
    /// Linux holds a child systemd-inhibit process whose PID is
    /// stored here and SIGTERM'd to release.
#if defined(_WIN32)
    bool                                             sleep_inhibited_      = false;
#else
    int                                              sleep_inhibitor_pid_  = 0;
#endif
    bool                                             v_swallowed_       = false;
    bool                                             paste_pending_     = false;
    bool                                             paste_ctrl_was_held_for_inject_ = false;

    /// @brief Active file-transfer senders keyed by
    /// transfer_id. Each sender owns its background thread;
    /// finished senders self-erase via on_finished. Held under
    /// @ref file_senders_m_ so spawning + completion can
    /// mutate concurrently.
    mutable std::mutex                               file_senders_m_;
    std::unordered_map<std::uint64_t,
                       std::unique_ptr<FileTransferSender>> file_senders_;
    std::unique_ptr<FileTransferReceiver>            file_transfer_receiver_;
    std::unique_ptr<platform::ITransferOverlay>      transfer_overlay_;
    std::unique_ptr<CursorRouter>                    cursor_router_;
    std::unique_ptr<input::CursorPoller>             cursor_poller_;
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

    /// @brief Per-workspace alone-online tracking. The detector
    /// runs on every peer-event tick + every workspace-merge,
    /// computes "online_count(ws) for the local PC", and emits
    /// `on_workspace_alone_changed` on transition. The "answered"
    /// set holds workspaces the user has clicked Stay on for the
    /// current alone-state — cleared on the Alone → NotAlone
    /// transition. Both maps are session-local; never persisted,
    /// never synced. Held under @ref alone_m_ so the detector
    /// (worker thread) doesn't race the UI thread's reads.
    mutable std::mutex                               alone_m_;
    std::unordered_map<std::string, bool>            alone_state_;
    std::unordered_set<std::string>                  alone_answered_;

    /// @brief Workspaces we've already seeded a default layout
    /// for in this session. Prevents the auto-fill from re-firing
    /// on every announce-driven on_changed callback (which would
    /// loop because set_layout itself triggers another on_changed).
    std::unordered_set<std::string>                  auto_layout_attempted_;

    /// @brief Cursor-arbitration state. The poller broadcasts
    /// only when the local cursor genuinely moved by user
    /// action — these atomics let us distinguish that from a
    /// move our own inject path just produced.
    std::atomic<std::int32_t>                        last_injected_x_{INT32_MIN};
    std::atomic<std::int32_t>                        last_injected_y_{INT32_MIN};
    std::atomic<std::int64_t>                        last_remote_move_ms_{0};

    /// @brief Owns the dormant-mode polled-cursor delta
    /// forwarder. Built in wire_raw_input_capture once the
    /// input_backend + cursor_router are live; null on a peer
    /// that doesn't have a usable input backend.
    std::unique_ptr<input::PolledMotionForwarder>    motion_forwarder_;

    /// @brief Translates discovery events into mesh + UI side
    /// effects. Borrows references to several members above —
    /// declared last so its references are valid by the time
    /// its constructor body runs.
    PeerEventHandler                                 peer_events_;
};

}  // namespace xorio::orchestrator::detail
