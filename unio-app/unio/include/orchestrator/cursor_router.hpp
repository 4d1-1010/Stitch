/// @file cursor_router.hpp
/// @brief Owner of the "is the cursor here?" state machine.
///
/// Scope: edge-detection + handoff over the control channel. This
/// is the bridge between the local cursor poller and the network:
///   * When this peer is *active* and the cursor crosses a local
///     monitor edge that maps to a peer's display, fire a Handoff
///     and become *dormant*.
///   * When dormant, the poller's outputs are ignored — the cursor
///     stays put on this PC because it's not the source.
///   * On receiving a Handoff, become active and warp the local
///     cursor to the entry point.
///
/// The router holds no transport — the orchestrator wires it to
/// the control channel + input backend via simple callbacks. That
/// keeps this file pure logic and easy to unit-test later.
///
/// Threading: methods are called from the cursor-poller thread and
/// the control-channel reader thread. All shared state is guarded
/// by the internal mutex.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace unio_ui::orchestrator {

/// @brief One monitor rectangle. Carries both the local OS
/// position (where the cursor lives in this peer's screen
/// coordinate space) and the mesh-global position (where the
/// user has arranged the monitor in the Layout canvas). The
/// router translates between the two when the cursor approaches
/// an edge so adjacency follows the user-arranged layout.
struct RouterMonitor {
    std::string  machine_id;
    std::int32_t global_x = 0;   ///< Position in mesh-global coords (after layout).
    std::int32_t global_y = 0;
    std::int32_t local_x  = 0;   ///< Original OS-local position (only used for the local peer).
    std::int32_t local_y  = 0;
    std::int32_t width    = 0;
    std::int32_t height   = 0;
};

class CursorRouter {
public:
    /// @brief Fired when the active peer hands the cursor off.
    /// @p target  receiver's machine_id.
    /// @p entry_x / entry_y  coords *in the receiver's local OS
    /// virtual screen space*. The router clamps before warping.
    using OnHandoffSendFn = std::function<void(const std::string& target,
                                                 std::int32_t entry_x,
                                                 std::int32_t entry_y)>;

    /// @brief Fired when this peer takes the cursor — the
    /// orchestrator warps the local cursor to (@p x, @p y) via
    /// the input backend.
    using OnWarpLocalFn = std::function<void(std::int32_t x,
                                              std::int32_t y)>;

    /// @brief Fired while this peer is *dormant* and the user is
    /// still moving the physical mouse: the router pins the local
    /// cursor at the handoff edge and forwards the user's frame-
    /// to-frame deltas to the peer that owns the cursor. The
    /// orchestrator wraps these in a MouseRel control frame.
    using OnForwardMotionFn =
        std::function<void(const std::string& target,
                            std::int32_t dx,
                            std::int32_t dy)>;

    /// @param local_machine_id  Identifies us in the peer strip.
    /// @param on_handoff_send   Invoked when we hand off.
    /// @param on_warp_local     Invoked when we take the cursor.
    /// @param on_forward_motion Invoked per frame while dormant
    ///                          to forward the user's motion
    ///                          deltas to the active peer.
    CursorRouter(std::string         local_machine_id,
                 OnHandoffSendFn     on_handoff_send,
                 OnWarpLocalFn       on_warp_local,
                 OnForwardMotionFn   on_forward_motion);

    /// @brief Update the router's view of every peer's monitor
    /// rects. Indexed by machine_id; the router looks up the
    /// receiver's monitors when computing the entry point.
    void set_monitors(std::vector<RouterMonitor> monitors);

    /// @brief Cursor poller: "the cursor moved to (x, y) on me".
    /// If we're active and the new position is past a monitor edge
    /// that maps to a neighbour, fire the handoff.
    void on_local_cursor_move(std::int32_t local_x, std::int32_t local_y);

    /// @brief Control channel: "@p source says I now own the cursor."
    void on_remote_handoff(const std::string& source,
                            std::int32_t entry_x,
                            std::int32_t entry_y);

    /// @brief Apply a relative motion delta arriving from the
    /// peer that just handed us the cursor. Updates our internal
    /// tracked workspace position, warps the OS cursor to follow,
    /// and runs edge detection against the tracked position so
    /// touchpad-driver phantom events on the OS cursor (Synaptics
    /// / ELAN / Precision Touchpad on Windows laptops) can't
    /// trip a return handoff. Mirrors Barrier's client-side model:
    /// the local OS cursor is downstream of our state, never an
    /// input to it.
    void apply_remote_delta(std::int32_t dx, std::int32_t dy);

    /// @brief True when this peer is the active cursor source.
    bool is_local_active() const;

    /// @brief machine_id of the peer that currently owns the
    /// cursor while we're dormant — empty while we're active or
    /// before the first handoff. Used by the orchestrator to
    /// route forwarded button / scroll frames.
    std::string forward_target() const;

    /// @brief Force the router into a dormant state. Used at
    /// startup so a single peer doesn't claim the cursor before
    /// any peers are even visible.
    void force_dormant();

    /// @brief Update the per-workspace capability flags for the
    /// local peer. @p is_cursor_member controls whether local
    /// mouse motion can initiate a handoff (i.e. push the cursor
    /// onto another peer); @p is_keyboard_member controls
    /// whether local key events forward while we're dormant.
    /// Non-cursor-members can still *receive* a handoff and act
    /// as a destination — they just can't be the initiator from
    /// their own physical mouse.
    void set_local_member_flags(bool is_cursor_member,
                                 bool is_keyboard_member);

    /// @brief True while local key events should be forwarded to
    /// the active peer. Read by the orchestrator before sending
    /// a KeyEvent frame so we don't leak local typing onto a
    /// peer the user opted out of via the Keyboard checkbox.
    bool keyboard_forwardable() const;

    /// @brief Snapshot of the workspace member flags. Used by
    /// the orchestrator's grab-state sync — pointer grab is
    /// only meaningful for cursor members, keyboard grab only
    /// for keyboard members.
    bool is_cursor_member() const;
    bool is_keyboard_member() const;

private:
    /// @brief Bounds of every monitor whose machine_id matches
    /// @p mid, packed for the locked-section computations.
    std::vector<const RouterMonitor*> monitors_for_locked(
        const std::string& mid) const;

    mutable std::mutex          m_;
    std::string                 local_id_;
    OnHandoffSendFn             on_handoff_send_;
    OnWarpLocalFn               on_warp_local_;
    OnForwardMotionFn           on_forward_motion_;

    std::vector<RouterMonitor>  monitors_;
    bool                        active_         = true;
    /// @brief When dormant, the machine_id of the active peer we
    /// forward motion deltas to. Empty while active or before the
    /// first handoff.
    std::string                 forward_target_;
    /// @brief Local OS pixel where we pin the cursor while
    /// dormant — set to the cursor's position at the moment we
    /// fired the outbound handoff. The router warps the cursor
    /// back here on every poll so the user's continuing motion
    /// shows up as a clean delta.
    std::int32_t                pinned_local_x_ = 0;
    std::int32_t                pinned_local_y_ = 0;

    /// @brief Previous sample's local cursor position. Dormant
    /// deltas are computed against this, not against pinned, so
    /// the warp-back-to-pin doesn't itself look like user motion
    /// once the cursor is settled.
    std::int32_t                last_sample_x_  = 0;
    std::int32_t                last_sample_y_  = 0;

    /// @brief When we just issued a warp (entering dormant or
    /// re-pinning after a forwarded delta), wait until we see a
    /// cursor sample at the warp target before resuming delta
    /// forwarding. Without this gate the very-next poll lands
    /// before X has processed the warp and we'd forward the
    /// (huge) pre-warp position as user motion.
    bool                        warp_pending_       = false;
    std::int32_t                warp_target_x_      = 0;
    std::int32_t                warp_target_y_      = 0;
    /// @brief Bound on how many polls we wait for the warp to
    /// land before giving up and resyncing — otherwise a missed
    /// warp would silently kill all forwarding.
    int                         warp_pending_count_ = 0;

    /// @brief Per-workspace capability flags. Default to true so
    /// a router that has never been configured behaves like the
    /// pre-checkbox build. Updated via @ref set_local_member_flags.
    bool                        is_cursor_member_   = true;
    bool                        is_keyboard_member_ = true;
    /// @brief True when our active state was triggered by a
    /// received handoff (a remote peer sent the cursor to us).
    /// Cleared the moment we fire a handoff back. Lets a non-
    /// cursor-member peer still hand the cursor BACK to the
    /// peer that visited it — without unlocking handoffs from
    /// the unchecked peer's own local mouse.
    bool                        remotely_active_    = false;

    /// @brief Tracked cursor position in mesh-global coords while
    /// we're remotely active — i.e. the cursor is visiting us,
    /// driven by deltas the source peer forwards. Updated only
    /// in @ref apply_remote_delta. Edge detection runs against
    /// this position instead of the polled OS cursor so any
    /// touchpad-driver phantom corrections to the OS cursor
    /// don't trip a return handoff. Valid iff @ref tracked_valid_.
    /// @ref tracked_last_at_ stamps each apply_remote_delta so
    /// the active poll path can drop tracked mode after an
    /// idle timeout — when the source peer stops forwarding,
    /// the cursor is "home" again and the local user's
    /// hardware should drive it via the polled path.
    bool                        tracked_valid_     = false;
    std::int32_t                tracked_global_x_  = 0;
    std::int32_t                tracked_global_y_  = 0;
    std::chrono::steady_clock::time_point tracked_last_at_{};

    /// @brief Wall-clock of the last received handoff and the
    /// peer that sent it. We suppress *return* handoffs to that
    /// same peer for a short window so a small mouse drift
    /// while the user reaches for the keyboard doesn't ping-pong
    /// the cursor straight back. Crosses to *other* peers stay
    /// unaffected so the user can keep moving across the mesh.
    std::chrono::steady_clock::time_point last_received_at_{};
    std::string                            last_received_from_;
    /// @brief Sticky edge-hit flag — once a handoff fires, we
    /// don't send another until the cursor moves *off* the edge
    /// (mirrors the Python tree's `_edge_hit_sent` debounce so a
    /// user lingering at the edge doesn't ping-pong). Default
    /// `true` so the very first cursor sample after startup
    /// can't spuriously fire — the user has to move the cursor
    /// away from the edge once before any handoff is allowed.
    bool                        edge_hit_sent_  = true;
    /// @brief Pixel margin on each edge — cursor at this distance
    /// from an edge counts as "at the edge". Phase C MVP uses 4px
    /// to match the Python default; Phase E will read this from
    /// the active workspace's `cursor_edge_margin` setting.
    std::int32_t                edge_margin_    = 4;
};

}  // namespace unio_ui::orchestrator
