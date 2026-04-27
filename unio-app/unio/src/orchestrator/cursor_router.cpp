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

namespace unio_ui::orchestrator {

namespace {

/// @brief Inclusive-range clamp helper.
std::int32_t clamp32(std::int32_t v, std::int32_t lo, std::int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

}  // namespace

CursorRouter::CursorRouter(std::string       local_machine_id,
                            OnHandoffSendFn   on_handoff_send,
                            OnWarpLocalFn     on_warp_local,
                            OnForwardMotionFn on_forward_motion)
    : local_id_(std::move(local_machine_id)),
      on_handoff_send_(std::move(on_handoff_send)),
      on_warp_local_(std::move(on_warp_local)),
      on_forward_motion_(std::move(on_forward_motion)) {}

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

/// @brief Pixels past the receiver's facing edge to land the
/// warped cursor on. Sized to give the user well past the
/// edge-margin debounce: a tiny back-motion shouldn't rip the
/// cursor straight back across. Capped to monitor.dim/4 at the
/// call site so it stays sensible on small displays.
constexpr std::int32_t kEntryInset = 120;
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
            // Dormant: forward the user's per-tick mouse delta
            // to the peer that owns the cursor. Compute the
            // delta against the previous sample (not against
            // the pin) so the warp-back doesn't itself look
            // like user motion. Only re-pin when the cursor has
            // wandered close to the OS edge — most ticks just
            // forward at the full poll rate, no warp.
            if (forward_target_.empty()) return;

            const std::int32_t dx = local_x - last_sample_x_;
            const std::int32_t dy = local_y - last_sample_y_;
            last_sample_x_ = local_x;
            last_sample_y_ = local_y;

            // Filter warp echoes — a single-tick delta this
            // large is almost certainly the cursor jumping due
            // to our own re-pin warp (or a delayed handoff
            // warp), not the user's hand. The user's hand moves
            // at most a few hundred pixels per 16 ms tick, so
            // anything >500 is the cursor teleporting back to
            // pinned and we drop it on the floor.
            constexpr std::int32_t kSpuriousDelta = 500;
            if (std::abs(dx) > kSpuriousDelta
                || std::abs(dy) > kSpuriousDelta) {
                return;
            }
            if (dx == 0 && dy == 0) return;

            forward_to = forward_target_;
            forward_dx = dx;
            forward_dy = dy;
            do_forward = true;

            // Re-pin only when the cursor has drifted far enough
            // from the centre that another tick of motion could
            // push it past the local OS edge (where the
            // absolute-position poller would lose the user's
            // continuing hand motion). Most ticks the cursor
            // stays near the centre and we forward at the full
            // poll rate without paying for a warp round-trip.
            constexpr std::int32_t kRePinThreshold = 300;
            const std::int32_t dist_x =
                std::abs(local_x - pinned_local_x_);
            const std::int32_t dist_y =
                std::abs(local_y - pinned_local_y_);
            if (dist_x > kRePinThreshold || dist_y > kRePinThreshold) {
                pin_warp_x     = pinned_local_x_;
                pin_warp_y     = pinned_local_y_;
                last_sample_x_ = pinned_local_x_;
                last_sample_y_ = pinned_local_y_;
                do_pin_warp    = true;
            }
        } else {

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
        // Per-workspace Cursor checkbox gate: if local is NOT a
        // cursor member, we only allow firing when we're being
        // remotely controlled (cursor was placed here by another
        // peer's handoff). That lets the visiting cursor leave
        // back to the source while still preventing the local
        // user's own mouse from pushing the cursor onto other
        // peers.
        if (!is_cursor_member_ && !remotely_active_) {
            edge_hit_sent_ = true;
            return;
        }
        // (No post-receive lockout — it caused legitimate
        // cross-back fires to be silently dropped, which felt
        // like the cursor was stuck on the receiving peer.
        // The kEntryInset alone gives the user enough buffer
        // against the most common drift pattern.)

        // Entry point in global coords — just past the target's
        // facing edge (kEntryInset pixels) so the receiver doesn't
        // immediately re-detect itself at an edge and ping-pong.
        switch (edge) {
            case Edge::Right:
                entry_x = clamp32(hit->global_x + kEntryInset,
                                   hit->global_x,
                                   hit->global_x + hit->width - 1);
                entry_y = clamp32(gy, hit->global_y,
                                   hit->global_y + hit->height - 1);
                break;
            case Edge::Left:
                entry_x = clamp32(hit->global_x + hit->width - 1 - kEntryInset,
                                   hit->global_x,
                                   hit->global_x + hit->width - 1);
                entry_y = clamp32(gy, hit->global_y,
                                   hit->global_y + hit->height - 1);
                break;
            case Edge::Bottom:
                entry_y = clamp32(hit->global_y + kEntryInset,
                                   hit->global_y,
                                   hit->global_y + hit->height - 1);
                entry_x = clamp32(gx, hit->global_x,
                                   hit->global_x + hit->width - 1);
                break;
            case Edge::Top:
                entry_y = clamp32(hit->global_y + hit->height - 1 - kEntryInset,
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
        // Mark this active spell as triggered by a remote peer.
        // Lets a non-cursor-member peer fire a handoff back to
        // the source while the cursor is visiting; cleared as
        // soon as the cursor leaves us again.
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
    }
    if (on_warp_local_) on_warp_local_(local_x, local_y);
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

}  // namespace unio_ui::orchestrator
