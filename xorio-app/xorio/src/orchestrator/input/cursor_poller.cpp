/// @file cursor_poller.cpp
/// @brief Implementation of @ref CursorPoller — single thread,
/// blocking sleep between samples, fires on real movement only.

#include "orchestrator/input/cursor_poller.hpp"

#include <utility>

namespace xorio::orchestrator::input {

CursorPoller::CursorPoller(IInputBackend& backend,
                            OnMoveFn       on_move,
                            OnButtonFn     on_button)
    : backend_(backend),
      on_move_(std::move(on_move)),
      on_button_(std::move(on_button)) {}

CursorPoller::~CursorPoller() {
    stop();
}

void CursorPoller::start(std::chrono::milliseconds interval) {
    if (running_.load(std::memory_order_acquire)) return;
    interval_ = interval;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&CursorPoller::run_loop, this);
}

void CursorPoller::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void CursorPoller::run_loop() {
    // Fire on every successful sample, even if the position
    // hasn't changed — the router's dormant-mode forwarder
    // warps the cursor back to a pinned spot every tick, which
    // means the very-next poll lands on the pinned coordinate
    // and a position-change filter here would suppress the
    // user's continuing motion. The active path's edge check
    // is cheap so the per-tick cost is negligible.
    std::uint32_t prev_buttons = backend_.get_button_mask();
    while (running_.load(std::memory_order_acquire)) {
        std::int32_t x = 0, y = 0;
        if (backend_.get_cursor_pos(x, y)) {
            if (on_move_) on_move_(x, y);
        }
        const std::uint32_t mask = backend_.get_button_mask();
        if (mask != prev_buttons && on_button_) {
            // Emit one callback per changed bit. Bits are
            // ordered Left, Middle, Right per IInputBackend.
            const std::uint32_t changed = mask ^ prev_buttons;
            constexpr MouseButton kBtns[] = {
                MouseButton::Left,
                MouseButton::Middle,
                MouseButton::Right,
            };
            for (std::uint32_t i = 0; i < 3; ++i) {
                if (changed & (1u << i)) {
                    on_button_(kBtns[i], (mask & (1u << i)) != 0);
                }
            }
            prev_buttons = mask;
        }
        std::this_thread::sleep_for(interval_);
    }
}

}  // namespace xorio::orchestrator::input
