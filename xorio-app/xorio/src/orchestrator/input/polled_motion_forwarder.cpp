/// @file polled_motion_forwarder.cpp
/// @brief Implementation of @ref PolledMotionForwarder. The body
/// is the dormant-mode forwarding logic that used to live inline
/// in the orchestrator's wire_raw_input_capture lambda.

#include "orchestrator/input/polled_motion_forwarder.hpp"

#include "orchestrator/cursor_router.hpp"
#include "orchestrator/input/input_backend.hpp"

#include <cstdlib>
#include <utility>

namespace xorio_ui::orchestrator::input {

PolledMotionForwarder::PolledMotionForwarder(IInputBackend& backend,
                                               CursorRouter&  router,
                                               SendDeltaFn    send_delta)
    : backend_(backend),
      router_(router),
      send_delta_(std::move(send_delta)) {}

void PolledMotionForwarder::on_hardware_motion(std::int32_t /*dx*/,
                                                 std::int32_t /*dy*/) {
    // Tell the router a real local-hardware event fired so
    // tracked-receiver mode drops back to polled the moment the
    // local user starts driving (only for cursor members; an
    // unchecked peer stays in tracked mode for the whole
    // visit).
    router_.note_local_hardware_motion();

    // On an unchecked peer where the cursor is currently
    // visiting, the local user can move the cursor with their
    // own mouse — but the next remote delta from the source
    // peer would otherwise warp it back to where tracked
    // thinks it is. Sync tracked to the polled position so
    // the remote stream picks up from where the local user
    // left it.
    std::int32_t cx = 0, cy = 0;
    if (!backend_.get_cursor_pos(cx, cy)) return;
    router_.sync_tracked_to_polled(cx, cy);

    // Forward only while dormant with a peer to forward to.
    const auto target = router_.forward_target();
    if (target.empty()) return;

    // First time we see polled — record and skip; the next
    // event delivers the first real delta.
    if (!last_polled_valid_) {
        last_polled_x_     = cx;
        last_polled_y_     = cy;
        last_polled_valid_ = true;
        return;
    }

    const std::int32_t pdx = cx - last_polled_x_;
    const std::int32_t pdy = cy - last_polled_y_;
    last_polled_x_ = cx;
    last_polled_y_ = cy;

    // Sanity bound: a single per-event polled advance over a
    // few hundred pixels means our reference is stale (cursor
    // crossed monitors, polled raced a recent warp). Drop and
    // let the next event re-baseline.
    constexpr std::int32_t kMaxPolledDelta = 200;
    if (std::abs(pdx) > kMaxPolledDelta
        || std::abs(pdy) > kMaxPolledDelta) {
        return;
    }
    if (pdx == 0 && pdy == 0) return;

    send_delta_(target, pdx, pdy);

    // Keep polled away from this monitor's OS edge so the user
    // can keep driving the receiver's cursor past where our own
    // screen would otherwise stop them. pin_warp_target returns
    // the local-monitor centre iff polled sits within a small
    // band of any edge.
    std::int32_t pwx = 0, pwy = 0;
    if (router_.pin_warp_target(cx, cy, /*edge_threshold=*/50,
                                  pwx, pwy)) {
        backend_.inject_mouse_move(pwx, pwy);
        // Invalidate so the next event re-baselines against
        // post-warp polled — without this the next polled-delta
        // would compute (current_polled - warp_target) before
        // the OS settles the warp.
        last_polled_valid_ = false;
    }
}

void PolledMotionForwarder::invalidate_polled_reference() {
    last_polled_valid_ = false;
}

}  // namespace xorio_ui::orchestrator::input
