/// @file polled_motion_forwarder.hpp
/// @brief Source-side dormant-mode mouse forwarding helper.
///
/// Scope: takes raw HID motion events from the platform raw-input
/// listener, computes the polled-cursor advance since the last
/// event, and emits a delta to the active peer through a user-
/// supplied send callback. Pin-warps the OS cursor back to the
/// local monitor's centre when polled approaches the screen
/// edge so the user can keep driving the receiver's cursor
/// past their own monitor's bounds.
///
/// Why polled-delta and not the raw HID delta: the OS clamps
/// polled at the source monitor's edge, so a fast cross from a
/// small monitor to a wide receiver doesn't race the receiver's
/// cursor past the entry inset to the far edge. Pin-warp keeps
/// polled in a free-motion range so the user can keep pushing
/// in the same direction.
///
/// Threading: called from the raw-input listener thread. Holds
/// no locks of its own — `last_polled_*` state is only touched
/// from that one thread. The cursor_router methods it calls
/// are themselves locked.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace xorio_ui::orchestrator {

class CursorRouter;

namespace input {

class IInputBackend;

class PolledMotionForwarder {
public:
    /// @brief Invoked with the destination peer's machine_id
    /// and the polled-cursor advance to forward.
    using SendDeltaFn =
        std::function<void(const std::string& target,
                            std::int32_t dx,
                            std::int32_t dy)>;

    PolledMotionForwarder(IInputBackend& backend,
                           CursorRouter&  router,
                           SendDeltaFn    send_delta);

    PolledMotionForwarder(const PolledMotionForwarder&)            = delete;
    PolledMotionForwarder& operator=(const PolledMotionForwarder&) = delete;

    /// @brief Process a raw hardware motion event. Notes the
    /// event on the router (so any active tracked-receiver mode
    /// drops back to polled), then — if we're dormant with a
    /// forward target — queries polled, computes the polled-
    /// delta vs. our last sample, forwards via send_delta, and
    /// pin-warps polled back to monitor centre when it gets
    /// near an edge. The raw @p dx / @p dy are kept in the
    /// signature for future filtering work; they aren't used
    /// directly today.
    void on_hardware_motion(std::int32_t dx, std::int32_t dy);

    /// @brief Drop the cached polled-cursor reference. Call
    /// after any local cursor warp (handoff entry, pin-warp,
    /// tracked re-warp) so the next on_hardware_motion event
    /// re-baselines against the new polled value instead of
    /// computing a giant `current_polled - stale_warp_target`
    /// delta during the X11 race window where polled hasn't
    /// yet caught up to the warp.
    void invalidate_polled_reference();

private:
    IInputBackend& backend_;
    CursorRouter&  router_;
    SendDeltaFn    send_delta_;

    std::int32_t last_polled_x_     = 0;
    std::int32_t last_polled_y_     = 0;
    bool         last_polled_valid_ = false;
};

}  // namespace input
}  // namespace xorio_ui::orchestrator
