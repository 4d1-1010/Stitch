/// @file cursor_poller.hpp
/// @brief Background thread that polls the local cursor and emits
/// a callback whenever it moves.
///
/// Scope: pure poll loop — no edge detection, no peer fan-out.
/// The orchestrator wires the callback to its control channel
/// (Phase B today; the Phase C cursor router will inject itself
/// between this and the channel to add edge-handoff). Keeping the
/// poller dedicated to cursor sampling means the same class can
/// later drive button + scroll capture without touching the
/// fan-out logic.
///
/// Threading: owns a single polling thread. start() / stop() are
/// idempotent; the destructor stops cleanly.
#pragma once

#include "orchestrator/input/input_backend.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace unio_ui::orchestrator::input {

class CursorPoller {
public:
    /// @brief Fired (from the polling thread) every time the
    /// local cursor's position changes. Coordinates are local
    /// OS virtual-screen pixels — translation to mesh-global
    /// coords happens upstream.
    using OnMoveFn = std::function<void(std::int32_t x, std::int32_t y)>;

    /// @brief Fired on every button-state transition — pressed
    /// goes from 0→1 or back. The poller delivers per-button
    /// edges so the orchestrator can forward a clean
    /// press/release pair while we're the dormant peer.
    using OnButtonFn = std::function<void(MouseButton button,
                                            bool pressed)>;

    /// @param backend    Borrowed reference to the open input
    ///                   backend; CursorPoller does not own it.
    /// @param on_move    Callback invoked on every successful sample.
    /// @param on_button  Callback invoked on each button transition.
    CursorPoller(IInputBackend& backend,
                 OnMoveFn       on_move,
                 OnButtonFn     on_button = {});

    ~CursorPoller();

    CursorPoller(const CursorPoller&)            = delete;
    CursorPoller& operator=(const CursorPoller&) = delete;

    /// @brief Spawn the polling thread (if not already running).
    /// @param interval  Sampling period; ~16 ms = 60 Hz.
    void start(std::chrono::milliseconds interval =
               std::chrono::milliseconds(16));

    /// @brief Stop + join the polling thread. Safe to call from
    /// any thread; idempotent.
    void stop();

private:
    void run_loop();

    IInputBackend&            backend_;
    OnMoveFn                  on_move_;
    OnButtonFn                on_button_;
    std::chrono::milliseconds interval_{16};
    std::thread               thread_;
    std::atomic<bool>         running_{false};
};

}  // namespace unio_ui::orchestrator::input
