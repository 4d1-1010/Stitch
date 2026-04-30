/// @file cursor_router.cpp
/// @brief Implementation of @ref CursorRouter — pure state-machine,
/// no I/O. Edge crossings translate to Handoff send-callback
/// invocations; received handoffs translate to warp-local
/// callback invocations. Strip = alphabetically-sorted list of
/// machine_ids participating in the active workspace.

#include "orchestrator/cursor_router.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <utility>

namespace xorio_ui::orchestrator {

namespace {

/// @brief Inclusive-range clamp helper.
std::int32_t clamp32(std::int32_t v, std::int32_t lo, std::int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

CursorRouter::CursorRouter(std::string         local_machine_id,
                            OnHandoffSendFn    on_handoff_send,
                            OnWarpLocalFn      on_warp_local,
                            OnForwardMotionFn  on_forward_motion,
                            OnRelativeMotionFn on_relative_motion)
    : local_id_(std::move(local_machine_id)),
      on_handoff_send_(std::move(on_handoff_send)),
      on_warp_local_(std::move(on_warp_local)),
      on_forward_motion_(std::move(on_forward_motion)),
      on_relative_motion_(std::move(on_relative_motion)) {}

void CursorRouter::set_monitors(std::vector<RouterMonitor> monitors) {
    std::lock_guard lk(m_);
    monitors_ = std::move(monitors);
}

bool CursorRouter::is_local_active() const {
    std::lock_guard lk(m_);
    return active_;
}

void CursorRouter::force_dormant() {
    std::lock_guard lk(m_);
    active_ = false;
}

std::string CursorRouter::forward_target() const {
    std::lock_guard lk(m_);
    return active_ ? std::string{} : forward_target_;
}

void CursorRouter::set_local_member_flags(bool is_cursor_member,
                                            bool is_keyboard_member) {
    std::lock_guard lk(m_);
    const bool was_member = is_cursor_member_;
    is_cursor_member_   = is_cursor_member;
    is_keyboard_member_ = is_keyboard_member;

    // Cursor membership just got revoked while we were the
    // dormant peer (i.e. the global cursor lives somewhere
    // else and we were forwarding our mouse to it). The
    // global cursor stays where it is — but we stop being
    // a forwarder, reclaim our local cursor, and let the
    // user drive it locally with their own mouse.
    if (was_member && !is_cursor_member && !active_) {
        active_             = true;
        forward_target_.clear();
        remotely_active_    = false;
        warp_pending_       = false;
        warp_pending_count_ = 0;
        // Suppress the very-next edge fire — the cursor
        // just popped back into existence and is likely
        // already hugging the edge it left through.
        edge_hit_sent_      = true;
    }
}

void CursorRouter::set_edge_margin(std::int32_t margin) {
    std::lock_guard lk(m_);
    edge_margin_ = std::max(margin, 1);
}

void CursorRouter::set_require_modifier(bool require) {
    std::lock_guard lk(m_);
    require_modifier_ = require;
}

void CursorRouter::set_modifier_held(bool held) {
    std::lock_guard lk(m_);
    modifier_held_ = held;
}

void CursorRouter::on_peer_lost(const std::string& machine_id) {
    std::lock_guard lk(m_);
    const bool was_forwarding_to_peer =
        !active_ && forward_target_ == machine_id;
    const bool was_visited_by_peer =
        remotely_active_ && last_received_from_ == machine_id;
    if (!was_forwarding_to_peer && !was_visited_by_peer) return;
    std::fprintf(stderr,
                 "router: peer %s lost — reclaiming active state "
                 "(was_forward=%d was_visit=%d)\n",
                 machine_id.c_str(),
                 was_forwarding_to_peer ? 1 : 0,
                 was_visited_by_peer ? 1 : 0);
    active_             = true;
    forward_target_.clear();
    remotely_active_    = false;
    tracked_valid_      = false;
    warp_pending_       = false;
    warp_pending_count_ = 0;
    // Suppress the very-next edge fire — the local cursor was
    // pinned at the dormant centre / inset and is likely still
    // hugging the edge it left through. Without this the router
    // would immediately re-fire a handoff to a peer that just
    // dropped, even before any real user input.
    edge_hit_sent_      = true;
}

bool CursorRouter::pin_warp_target(std::int32_t local_x,
                                     std::int32_t local_y,
                                     std::int32_t edge_threshold,
                                     std::int32_t& warp_local_x,
                                     std::int32_t& warp_local_y) const {
    std::lock_guard lk(m_);
    const auto locals = monitors_for_locked(local_id_);
    for (const auto* mon : locals) {
        if (local_x >= mon->local_x
            && local_x <  mon->local_x + mon->width
            && local_y >= mon->local_y
            && local_y <  mon->local_y + mon->height) {
            const bool near_edge =
                local_x <  mon->local_x + edge_threshold
                || local_x >= mon->local_x + mon->width - edge_threshold
                || local_y <  mon->local_y + edge_threshold
                || local_y >= mon->local_y + mon->height - edge_threshold;
            if (near_edge) {
                warp_local_x = mon->local_x + mon->width  / 2;
                warp_local_y = mon->local_y + mon->height / 2;
                return true;
            }
            return false;
        }
    }
    return false;
}

bool CursorRouter::keyboard_forwardable() const {
    std::lock_guard lk(m_);
    // We only forward when (a) we're a keyboard member AND
    // (b) we're dormant for cursor (i.e. the cursor lives on
    // another peer right now). Active-side typing lands locally
    // through the OS — no need to wire it.
    return is_keyboard_member_ && !active_ && !forward_target_.empty();
}

bool CursorRouter::is_cursor_member() const {
    std::lock_guard lk(m_);
    return is_cursor_member_;
}

bool CursorRouter::is_keyboard_member() const {
    std::lock_guard lk(m_);
    return is_keyboard_member_;
}

/// @brief Edge directions used by the adjacency check. Encoded so
/// each value's sign tells horizontal/vertical and direction.
namespace {
enum Edge { Right = 1, Left = -1, Bottom = 2, Top = -2 };

}  // namespace

void CursorRouter::on_local_cursor_move(std::int32_t local_x,
                                          std::int32_t local_y) {
    std::string  target;
    std::int32_t entry_x = 0;
    std::int32_t entry_y = 0;
    int          edge_log = 0;
    std::int32_t gx_log = 0, gy_log = 0;
    bool         saw_edge = false;
    // Dormant-mode forwarding state, filled inside the lock and
    // acted on outside it so callbacks don't re-enter.
    std::string  forward_to;
    std::int32_t forward_dx     = 0;
    std::int32_t forward_dy     = 0;
    std::int32_t pin_warp_x     = 0;
    std::int32_t pin_warp_y     = 0;
    bool         do_forward     = false;
    bool         do_pin_warp    = false;
    {
        std::lock_guard lk(m_);
        if (!active_) {
            // Dormant: nothing for the poller to do here.
            // Forwarding the user's mouse motion to the active
            // peer happens directly from the raw-input
            // listener (XInput2 RawMotion / Win32 RawInput
            // with the swallow-window ghost filter), wired in
            // the orchestrator. Polled-cursor batching at the
            // poll rate added perceptible latency on the
            // receiving end; per-event forwarding at the
            // device's native rate is smoother. Counter is
            // also reset by raw motion directly, no need to
            // touch it here.
            return;
        } else {
        // Active path. While tracked mode is engaged (cursor
        // was placed here by a remote handoff and the source
        // peer is actively forwarding deltas), skip polled-
        // cursor edge detection — apply_remote_delta runs the
        // edge check against our tracked workspace position,
        // immune to OS-cursor noise from touchpad-driver
        // phantom corrections.
        //
        // Tracked-mode exit conditions — only on a checked
        // (cursor-member) peer:
        //   1. Local hardware input here: any raw HID event
        //      noted since the last poll means the local user
        //      just started driving. Take over immediately so
        //      they can fire an active edge handoff on their
        //      first push.
        //   2. Idle timeout: 500 ms with no apply_remote_delta
        //      and no local hardware motion. Catches the case
        //      where the source peer just handed the cursor
        //      back here without the user touching any local
        //      device.
        // An unchecked peer's local mouse can't drive the
        // global cursor out and the visiting cursor's only
        // exit is the remote driver's tracked-edge fire — so
        // tracked mode stays armed on an unchecked peer for
        // the entire visit, regardless of idle gaps in the
        // remote driver's stream or local hardware noise.
        // Without this, a brief pause on the source peer
        // dropped tracked here and apply_remote_delta fell
        // back to plain relative-inject — losing the tracked-
        // edge logic that's the only way a visiting cursor
        // can leave an unchecked peer.
        if (tracked_valid_ && is_cursor_member_) {
            if (raw_motion_since_poll_ > 0) {
                tracked_valid_   = false;
                remotely_active_ = false;
            } else {
                const auto now = std::chrono::steady_clock::now();
                if (now - tracked_last_at_
                    > std::chrono::milliseconds(500)) {
                    tracked_valid_   = false;
                    remotely_active_ = false;
                } else {
                    return;
                }
            }
        } else if (tracked_valid_) {
            return;
        }

        // Find the LOCAL monitor under the cursor in OS coords.
        const auto locals = monitors_for_locked(local_id_);
        if (locals.empty()) return;

        const RouterMonitor* on_mon = nullptr;
        for (const auto* mon : locals) {
            if (local_x >= mon->local_x
                && local_x <  mon->local_x + mon->width
                && local_y >= mon->local_y
                && local_y <  mon->local_y + mon->height) {
                on_mon = mon;
                break;
            }
        }
        if (on_mon == nullptr) {
            for (const auto* mon : locals) {
                if (local_y >= mon->local_y
                    && local_y < mon->local_y + mon->height) {
                    on_mon = mon;
                    break;
                }
            }
        }
        if (on_mon == nullptr) return;

        // Translate to mesh-global so edge detection + adjacency
        // run in the user-arranged coordinate space.
        const std::int32_t gx =
            on_mon->global_x + (local_x - on_mon->local_x);
        const std::int32_t gy =
            on_mon->global_y + (local_y - on_mon->local_y);

        const std::int32_t mon_right = on_mon->global_x + on_mon->width;
        const std::int32_t mon_bot   = on_mon->global_y + on_mon->height;

        // Pick exactly one edge — horizontal wins over vertical
        // when the cursor is in a corner, matching Python's
        // _poll_active ordering. Edge-margin tolerance lets the
        // user trigger a touch before the OS clamp.
        int edge = 0;
        if      (gx <= on_mon->global_x + edge_margin_)  edge = Edge::Left;
        else if (gx >= mon_right - 1 - edge_margin_)     edge = Edge::Right;
        else if (gy <= on_mon->global_y + edge_margin_)  edge = Edge::Top;
        else if (gy >= mon_bot   - 1 - edge_margin_)     edge = Edge::Bottom;

        gx_log   = gx;
        gy_log   = gy;
        edge_log = edge;
        if (edge == 0) {
            edge_hit_sent_ = false;
            return;
        }
        saw_edge = true;
        if (edge_hit_sent_) return;
        // Workspace's "Hold Ctrl+Shift to cross" gate. When the
        // setting is on, a cursor at the edge without the
        // modifier held is a no-op — don't transition to dormant
        // and don't fire a handoff. State (active_, edge_hit_sent_)
        // stays unchanged so the very moment the user adds the
        // modifier the next poll fires through this gate normally.
        if (require_modifier_ && !modifier_held_) return;

        // Adjacency search — pick the monitor whose facing edge
        // is closest to ours in the cursor's direction of travel,
        // with the cursor's orthogonal coordinate falling inside
        // the candidate's range. The candidate must extend past
        // our edge (so it's actually in that direction) but its
        // facing edge can be on either side of ours: a tiny
        // overlap from canvas snapping or a small gap should
        // both pick the candidate the user clearly intended.
        //
        // Same-machine monitors stay in the candidate set as
        // *blockers* — if the cursor would naturally flow into
        // another local display in the OS, we let it, instead of
        // jumping the cursor to a remote peer.
        const RouterMonitor* hit       = nullptr;
        std::int32_t         best_dist = std::numeric_limits<std::int32_t>::max();
        for (const auto& m : monitors_) {
            const std::int32_t rl = m.global_x;
            const std::int32_t rr = m.global_x + m.width;
            const std::int32_t rt = m.global_y;
            const std::int32_t rb = m.global_y + m.height;
            std::int32_t d     = 0;
            bool         match = false;
            switch (edge) {
                case Edge::Right:
                    if (rr > mon_right && gy >= rt && gy < rb) {
                        d = std::abs(rl - mon_right); match = true;
                    }
                    break;
                case Edge::Left:
                    if (rl < on_mon->global_x && gy >= rt && gy < rb) {
                        d = std::abs(on_mon->global_x - rr); match = true;
                    }
                    break;
                case Edge::Bottom:
                    if (rb > mon_bot && gx >= rl && gx < rr) {
                        d = std::abs(rt - mon_bot); match = true;
                    }
                    break;
                case Edge::Top:
                    if (rt < on_mon->global_y && gx >= rl && gx < rr) {
                        d = std::abs(on_mon->global_y - rb); match = true;
                    }
                    break;
            }
            if (match && d < best_dist) {
                best_dist = d;
                hit       = &m;
            }
        }
        if (hit == nullptr) return;
        // If the closest neighbour is on this same machine, no
        // handoff: the OS will route the cursor into that local
        // display naturally. The router only fires when crossing
        // an edge that maps to a *remote* display.
        if (hit->machine_id == local_id_) {
            edge_hit_sent_ = true;  // suppress until cursor steps off
            return;
        }
        // Per-workspace Input checkbox gate: an unchecked peer's
        // local mouse can never push the global cursor outward
        // — even when the cursor is currently visiting from
        // another peer. Visiting cursor leaves only via the
        // remote driver's forwarded MouseRel walking the
        // receiver-side tracked position past an edge (handled
        // in apply_remote_delta). The local user keeps moving
        // their cursor freely; it just can't escape this PC.
        if (!is_cursor_member_) {
            edge_hit_sent_ = true;
            return;
        }
        // Entry point in global coords — exactly 2 * edge_margin
        // past the receiver's trigger zone (which is
        // 0..edge_margin from its facing edge). That gives the
        // cursor one full edge_margin of buffer between the
        // landing point and the receiver's own edge-trigger so
        // small back-drift on the receiver doesn't immediately
        // re-fire the return handoff.
        //
        // Tight margins (the slider's 4 px floor) leave only
        // 8 px of buffer; on Windows hosts where the touchpad
        // driver fires phantom-correction nudges right after
        // every SetCursorPos, that buffer can lose to the race
        // and the cursor ping-pongs across the edge. Users on
        // such hosts dial the slider up — the entry inset
        // scales with it (margin=20 → 40 px buffer, etc.).
        const std::int32_t inset = edge_margin_ * 2;
        switch (edge) {
            case Edge::Right:
                entry_x = clamp32(hit->global_x + inset,
                                   hit->global_x,
                                   hit->global_x + hit->width - 1);
                entry_y = clamp32(gy, hit->global_y,
                                   hit->global_y + hit->height - 1);
                break;
            case Edge::Left:
                entry_x = clamp32(hit->global_x + hit->width - 1 - inset,
                                   hit->global_x,
                                   hit->global_x + hit->width - 1);
                entry_y = clamp32(gy, hit->global_y,
                                   hit->global_y + hit->height - 1);
                break;
            case Edge::Bottom:
                entry_y = clamp32(hit->global_y + inset,
                                   hit->global_y,
                                   hit->global_y + hit->height - 1);
                entry_x = clamp32(gx, hit->global_x,
                                   hit->global_x + hit->width - 1);
                break;
            case Edge::Top:
                entry_y = clamp32(hit->global_y + hit->height - 1 - inset,
                                   hit->global_y,
                                   hit->global_y + hit->height - 1);
                entry_x = clamp32(gx, hit->global_x,
                                   hit->global_x + hit->width - 1);
                break;
        }

        target          = hit->machine_id;
        edge_hit_sent_  = true;
        if (is_cursor_member_) {
            // Checked peer: enter the standard dormant /
            // forwarding mode. Cursor hides, motion gets
            // forwarded to whoever now holds the cursor.
            active_         = false;
            forward_target_ = target;
            remotely_active_ = false;
            tracked_valid_   = false;
            // Pin at the originating monitor's CENTER (not the
            // edge we just crossed). The OS would clamp the
            // cursor at the edge otherwise, and the absolute-
            // position poller would never see further motion
            // in that direction — every poll past the clamp
            // would report the same pixel and the user's hand
            // motion would be silently dropped. Pinning at the
            // centre gives the cursor headroom in all four
            // directions.
            pinned_local_x_ = on_mon->local_x + on_mon->width  / 2;
            pinned_local_y_ = on_mon->local_y + on_mon->height / 2;
            pin_warp_x      = pinned_local_x_;
            pin_warp_y      = pinned_local_y_;
            do_pin_warp     = true;
            // Seed last_sample to where the cursor will be
            // after the pin warp lands, not where it is right
            // now — otherwise the centre warp itself leaks
            // across the wire as a huge bogus delta on the
            // very next poll.
            last_sample_x_ = pinned_local_x_;
            last_sample_y_ = pinned_local_y_;
        } else {
            // Unchecked peer: the visiting cursor is going
            // home. We stay active with our own cursor
            // visible and locally controlled — the user said
            // "the mouse on the PC with cursor unchecked …
            // remains visible on it (controlled) by its own
            // mouse" once the visit ends.
            remotely_active_ = false;
        }
        }   // end of else { (active path)
    }

    // Dormant-mode forwarding: outside the lock so the control-
    // channel + input-backend callbacks can't re-enter the router.
    // The pin warp only fires when the cursor has wandered far
    // enough from the centre (do_pin_warp set in the dormant
    // path) — otherwise we forward at the full poll rate.
    if (do_forward) {
        if (on_forward_motion_) {
            on_forward_motion_(forward_to, forward_dx, forward_dy);
        }
        if (do_pin_warp && on_warp_local_) {
            on_warp_local_(pin_warp_x, pin_warp_y);
        }
        return;
    }

    // Throttled diagnostic — fires at most once per 500ms when
    // the cursor is at any edge. Tells us why a handoff did or
    // didn't fire.
    if (saw_edge) {
        static std::atomic<std::int64_t> last_log_ms{0};
        const auto now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count();
        const auto last = last_log_ms.load(std::memory_order_acquire);
        if (now_ms - last > 500) {
            last_log_ms.store(now_ms, std::memory_order_release);
            const char* edge_name =
                edge_log == Edge::Left   ? "left"   :
                edge_log == Edge::Right  ? "right"  :
                edge_log == Edge::Top    ? "top"    :
                edge_log == Edge::Bottom ? "bottom" : "?";
            std::fprintf(stderr,
                         "router: edge=%s @ global (%d, %d) → %s\n",
                         edge_name, gx_log, gy_log,
                         target.empty() ? "no neighbour" : target.c_str());
        }
    }

    if (on_handoff_send_ && !target.empty()) {
        on_handoff_send_(target, entry_x, entry_y);
    }
    if (do_pin_warp && on_warp_local_) {
        on_warp_local_(pin_warp_x, pin_warp_y);
    }
}

void CursorRouter::on_remote_handoff(const std::string& source,
                                       std::int32_t entry_global_x,
                                       std::int32_t entry_global_y) {
    std::int32_t local_x = entry_global_x;
    std::int32_t local_y = entry_global_y;
    {
        std::lock_guard lk(m_);
        // Find the local monitor that contains the entry point in
        // mesh-global space, then translate global → local using
        // the same per-monitor offset the active path uses. If no
        // monitor matches (sender's coords outside our globe),
        // pick the monitor closest in y and clamp.
        const auto locals = monitors_for_locked(local_id_);
        const RouterMonitor* host = nullptr;
        for (const auto* mon : locals) {
            if (entry_global_x >= mon->global_x
                && entry_global_x <  mon->global_x + mon->width
                && entry_global_y >= mon->global_y
                && entry_global_y <  mon->global_y + mon->height) {
                host = mon;
                break;
            }
        }
        if (host == nullptr && !locals.empty()) {
            host = locals.front();
            std::int32_t best_dy = std::abs(
                (host->global_y + host->height / 2) - entry_global_y);
            for (const auto* mon : locals) {
                const std::int32_t dy = std::abs(
                    (mon->global_y + mon->height / 2) - entry_global_y);
                if (dy < best_dy) { host = mon; best_dy = dy; }
            }
        }
        if (host != nullptr) {
            const std::int32_t gx = clamp32(
                entry_global_x, host->global_x,
                host->global_x + host->width  - 1);
            const std::int32_t gy = clamp32(
                entry_global_y, host->global_y,
                host->global_y + host->height - 1);
            local_x = host->local_x + (gx - host->global_x);
            local_y = host->local_y + (gy - host->global_y);
        }
        active_        = true;
        // We're the active peer again — clear the dormant-mode
        // forwarding state so subsequent local cursor moves go
        // through the active edge-detection path, not the delta
        // forwarder.
        forward_target_.clear();
        warp_pending_       = false;
        warp_pending_count_ = 0;
        // Always enter tracked mode on a remote handoff. The
        // OS cursor's polled position is suspect on a receiver
        // — Win laptop touchpad drivers fire phantom-correction
        // events after every SetCursorPos, and the polled value
        // would let those bounce the cursor back to the source.
        // The active-poll path drops tracked mode after a 500
        // ms idle window with no apply_remote_delta — see
        // @ref on_local_cursor_move. Until then, edges fire
        // only from apply_remote_delta against the tracked
        // workspace position.
        remotely_active_ = true;
        // Stamp the receive time + source peer so the handoff-
        // fire path can ignore tiny mouse drift for a short
        // window after the visit starts — without this the user
        // reaching for the keyboard usually nudges the mouse
        // just enough to push the cursor right back across the
        // edge it came in. Scoped to the source peer so a fresh
        // cross to a *different* peer is never blocked.
        last_received_at_   = std::chrono::steady_clock::now();
        last_received_from_ = source;
        // Arm the debounce — the warp lands the cursor right at
        // the entry inset, which is within edge-margin distance
        // of the receiver's edge. Without this the receiver's
        // very next poll sample would re-detect itself at that
        // edge and ping-pong the cursor straight back. Cleared
        // when the user moves the cursor off the edge for real.
        edge_hit_sent_ = true;
        // Seed the tracked workspace position so subsequent
        // apply_remote_delta calls integrate against the entry
        // point. Edge detection while we're remotely active
        // runs against this tracked value rather than polled
        // OS cursor — that's how Barrier's client side avoids
        // touchpad-driver phantom corrections re-tripping the
        // edge: the OS cursor is downstream of our state, never
        // an input to it.
        tracked_global_x_ = entry_global_x;
        tracked_global_y_ = entry_global_y;
        tracked_valid_    = true;
        tracked_last_at_  = std::chrono::steady_clock::now();
    }
    if (on_warp_local_) on_warp_local_(local_x, local_y);
}

void CursorRouter::note_local_hardware_motion() {
    std::lock_guard lk(m_);
    // Saturating bump — the dormant poll path zeroes this on
    // each tick. We never need precise counts; just "did at
    // least one real event happen since the last poll".
    if (raw_motion_since_poll_ < 1000) {
        ++raw_motion_since_poll_;
    }
}

void CursorRouter::sync_tracked_to_polled(std::int32_t local_x,
                                            std::int32_t local_y) {
    std::lock_guard lk(m_);
    if (!tracked_valid_) return;
    // Only meaningful when the local user is allowed to move
    // the visiting cursor without stealing it (Input
    // unchecked). For checked peers the local-hardware-input
    // exit drops tracked anyway, so this would be undone on
    // the next poll.
    if (is_cursor_member_) return;

    // Translate local OS coords to mesh-global, using the
    // per-monitor offset of whichever local monitor the
    // cursor sits on.
    const auto locals = monitors_for_locked(local_id_);
    for (const auto* mon : locals) {
        if (local_x >= mon->local_x
            && local_x <  mon->local_x + mon->width
            && local_y >= mon->local_y
            && local_y <  mon->local_y + mon->height) {
            tracked_global_x_ =
                mon->global_x + (local_x - mon->local_x);
            tracked_global_y_ =
                mon->global_y + (local_y - mon->local_y);
            return;
        }
    }
}

void CursorRouter::apply_remote_delta(std::int32_t dx,
                                        std::int32_t dy) {
    std::string  target;
    std::int32_t entry_x = 0;
    std::int32_t entry_y = 0;
    bool         do_warp = false;
    std::int32_t warp_local_x = 0;
    std::int32_t warp_local_y = 0;
    bool         apply_relative = false;
    std::int32_t rel_dx = 0;
    std::int32_t rel_dy = 0;
    {
        std::lock_guard lk(m_);
        if (!tracked_valid_) {
            // Not in tracked mode: cursor is locally home,
            // we're just relaying a remote peer's mouse motion
            // to the local cursor. Apply the delta as a plain
            // relative move — no edge check, no tracked
            // bookkeeping, the active poll path handles edges
            // from the resulting polled-cursor position the
            // same as if it were our local user moving.
            apply_relative = true;
            rel_dx = dx;
            rel_dy = dy;
        } else {
        tracked_last_at_ = std::chrono::steady_clock::now();

        // Step the tracked position by the incoming delta. The
        // OS cursor follows below via on_warp_local_ — but the
        // edge check runs against tracked, so OS-cursor noise
        // (touchpad ghosts, the user nudging Diana's hardware
        // mid-visit) can't trip the return.
        tracked_global_x_ += dx;
        tracked_global_y_ += dy;

        // Resolve which local monitor the tracked position
        // currently sits on. Same lookup the active path runs
        // against polled local coords, just driven by tracked
        // global coords. If tracked drifts past every local
        // monitor (corner case: source forwards faster than we
        // can fire a return), pin to the closest monitor in y.
        const auto locals = monitors_for_locked(local_id_);
        if (locals.empty()) return;
        const RouterMonitor* on_mon = nullptr;
        for (const auto* mon : locals) {
            if (tracked_global_x_ >= mon->global_x
                && tracked_global_x_ <  mon->global_x + mon->width
                && tracked_global_y_ >= mon->global_y
                && tracked_global_y_ <  mon->global_y + mon->height) {
                on_mon = mon;
                break;
            }
        }
        if (on_mon == nullptr) {
            on_mon = locals.front();
            std::int32_t best_dy = std::abs(
                (on_mon->global_y + on_mon->height / 2)
                - tracked_global_y_);
            for (const auto* mon : locals) {
                const std::int32_t cdy = std::abs(
                    (mon->global_y + mon->height / 2)
                    - tracked_global_y_);
                if (cdy < best_dy) { on_mon = mon; best_dy = cdy; }
            }
        }
        // Edge detection — same edge_margin trigger as the active
        // path so the slider applies symmetrically in both
        // directions. Phantom-correction protection comes from
        // the de-arm gate below: edge_hit_sent_ stays asserted
        // (set on receive-handoff) until tracked has clearly
        // moved past the inset zone, so a small back-drift
        // landing inside the trigger zone right after a handoff
        // doesn't fire a return.
        const std::int32_t mon_right = on_mon->global_x + on_mon->width;
        const std::int32_t mon_bot   = on_mon->global_y + on_mon->height;
        int edge = 0;
        if      (tracked_global_x_ <= on_mon->global_x + edge_margin_) edge = Edge::Left;
        else if (tracked_global_x_ >= mon_right - 1 - edge_margin_)    edge = Edge::Right;
        else if (tracked_global_y_ <= on_mon->global_y + edge_margin_) edge = Edge::Top;
        else if (tracked_global_y_ >= mon_bot   - 1 - edge_margin_)    edge = Edge::Bottom;

        // Clamp tracked to the host monitor's bounds so we don't
        // drift off into "no monitor" territory; warps land on
        // the same host below.
        const std::int32_t gx = clamp32(
            tracked_global_x_, on_mon->global_x,
            on_mon->global_x + on_mon->width  - 1);
        const std::int32_t gy = clamp32(
            tracked_global_y_, on_mon->global_y,
            on_mon->global_y + on_mon->height - 1);
        tracked_global_x_ = gx;
        tracked_global_y_ = gy;
        warp_local_x = on_mon->local_x + (gx - on_mon->global_x);
        warp_local_y = on_mon->local_y + (gy - on_mon->global_y);
        do_warp = true;
        std::fprintf(stderr,
                     "tracked: gx=%d gy=%d mon=[%d,%d %dx%d] "
                     "edge=%d hit_sent=%d em=%d\n",
                     gx, gy, on_mon->global_x, on_mon->global_y,
                     on_mon->width, on_mon->height,
                     edge, edge_hit_sent_ ? 1 : 0, edge_margin_);
        // De-arm only when tracked is clearly past the 2*edge_margin
        // inset on every axis — i.e., the user has actively pushed
        // the cursor inward, past where it landed after the receive-
        // handoff (entry inset = 2*edge_margin). Without this, the
        // very next tick after handoff arrival would clear edge_hit_
        // sent_ (cursor at exactly the inset boundary, edge=0), and
        // a single phantom-correction delta back into the trigger
        // zone would fire a return handoff.
        const std::int32_t inset_dearm = edge_margin_ * 2;
        const bool clearly_inside =
               tracked_global_x_ >  on_mon->global_x + inset_dearm
            && tracked_global_x_ <  mon_right - 1 - inset_dearm
            && tracked_global_y_ >  on_mon->global_y + inset_dearm
            && tracked_global_y_ <  mon_bot - 1 - inset_dearm;
        if (clearly_inside) {
            edge_hit_sent_ = false;
        }
        // Same modifier gate as the active path — Hold Ctrl+Shift
        // applies symmetrically whether we're cursor source via
        // active edge detection or via tracked-mode return.
        const bool gate_open = !require_modifier_ || modifier_held_;
        if (edge != 0 && !edge_hit_sent_ && gate_open) {
            // Pick the neighbour monitor (same adjacency search
            // as the active path) and fire a handoff. The
            // visiting cursor is leaving us back to the network.
            std::fprintf(stderr,
                         "tracked-adj: edge=%d gx=%d gy=%d "
                         "monitors=%zu\n",
                         edge, gx, gy, monitors_.size());
            for (const auto& m : monitors_) {
                std::fprintf(stderr,
                             "tracked-adj-mon: %s [%d,%d %dx%d]\n",
                             m.machine_id.c_str(),
                             m.global_x, m.global_y,
                             m.width, m.height);
            }
            const RouterMonitor* hit       = nullptr;
            std::int32_t         best_dist =
                std::numeric_limits<std::int32_t>::max();
            for (const auto& m : monitors_) {
                const std::int32_t rl = m.global_x;
                const std::int32_t rr = m.global_x + m.width;
                const std::int32_t rt = m.global_y;
                const std::int32_t rb = m.global_y + m.height;
                std::int32_t d     = 0;
                bool         match = false;
                switch (edge) {
                    case Edge::Right:
                        if (rr > mon_right && gy >= rt && gy < rb) {
                            d = std::abs(rl - mon_right); match = true;
                        }
                        break;
                    case Edge::Left:
                        if (rl < on_mon->global_x && gy >= rt && gy < rb) {
                            d = std::abs(on_mon->global_x - rr); match = true;
                        }
                        break;
                    case Edge::Bottom:
                        if (rb > mon_bot && gx >= rl && gx < rr) {
                            d = std::abs(rt - mon_bot); match = true;
                        }
                        break;
                    case Edge::Top:
                        if (rt < on_mon->global_y && gx >= rl && gx < rr) {
                            d = std::abs(on_mon->global_y - rb); match = true;
                        }
                        break;
                }
                if (match && d < best_dist) {
                    best_dist = d;
                    hit       = &m;
                }
            }
            if (hit != nullptr && hit->machine_id != local_id_) {
                const std::int32_t inset = std::max(edge_margin_, 1) * 2;
                switch (edge) {
                    case Edge::Right:
                        entry_x = clamp32(hit->global_x + inset,
                                           hit->global_x,
                                           hit->global_x + hit->width - 1);
                        entry_y = clamp32(gy, hit->global_y,
                                           hit->global_y + hit->height - 1);
                        break;
                    case Edge::Left:
                        entry_x = clamp32(hit->global_x + hit->width - 1 - inset,
                                           hit->global_x,
                                           hit->global_x + hit->width - 1);
                        entry_y = clamp32(gy, hit->global_y,
                                           hit->global_y + hit->height - 1);
                        break;
                    case Edge::Bottom:
                        entry_y = clamp32(hit->global_y + inset,
                                           hit->global_y,
                                           hit->global_y + hit->height - 1);
                        entry_x = clamp32(gx, hit->global_x,
                                           hit->global_x + hit->width - 1);
                        break;
                    case Edge::Top:
                        entry_y = clamp32(hit->global_y + hit->height - 1 - inset,
                                           hit->global_y,
                                           hit->global_y + hit->height - 1);
                        entry_x = clamp32(gx, hit->global_x,
                                           hit->global_x + hit->width - 1);
                        break;
                }
                target           = hit->machine_id;
                edge_hit_sent_   = true;
                tracked_valid_   = false;
                remotely_active_ = false;
                // Visiting cursor is leaving — we go back to
                // dormant. Forward target = the peer we're
                // sending the cursor to (typically the original
                // source). Clears in on_remote_handoff if the
                // cursor returns later.
                active_         = false;
                forward_target_ = target;
                // Seed pinned + last_sample to the local
                // monitor centre so the dormant poll path has
                // a known reference point. Without this the
                // first dormant poll computes (polled - stale
                // last_sample) — an unbounded bogus delta —
                // and the pin-warp threshold check (against a
                // stale pinned) can warp the OS cursor to a
                // garbage location, leaving the user unable
                // to drive the cursor on the new active peer
                // until the state stabilises. Mirrors what the
                // active-path edge fire already does for the
                // mirror case (cross out via local mouse).
                pinned_local_x_ = on_mon->local_x + on_mon->width  / 2;
                pinned_local_y_ = on_mon->local_y + on_mon->height / 2;
                last_sample_x_  = pinned_local_x_;
                last_sample_y_  = pinned_local_y_;
                warp_local_x    = pinned_local_x_;
                warp_local_y    = pinned_local_y_;
                do_warp         = true;
                std::fprintf(stderr,
                             "router: tracked-edge=%s @ global (%d, %d) → %s\n",
                             edge == Edge::Left   ? "left"   :
                             edge == Edge::Right  ? "right"  :
                             edge == Edge::Top    ? "top"    : "bottom",
                             gx, gy, target.c_str());
            }
        }
        }   // end of else { (tracked path)
    }
    if (apply_relative && on_relative_motion_) {
        on_relative_motion_(rel_dx, rel_dy);
    }
    if (do_warp && on_warp_local_) {
        on_warp_local_(warp_local_x, warp_local_y);
    }
    if (!target.empty() && on_handoff_send_) {
        on_handoff_send_(target, entry_x, entry_y);
    }
}

std::vector<const RouterMonitor*>
CursorRouter::monitors_for_locked(const std::string& mid) const {
    std::vector<const RouterMonitor*> out;
    out.reserve(monitors_.size());
    for (const auto& mon : monitors_) {
        if (mon.machine_id == mid) out.push_back(&mon);
    }
    return out;
}

}  // namespace xorio_ui::orchestrator
