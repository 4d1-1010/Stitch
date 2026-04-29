/// @file clipboard_monitor.hpp
/// @brief Background thread that polls the local clipboard,
/// detects user-initiated changes, and fires a callback. Echo
/// suppression: when the orchestrator writes to the clipboard
/// in response to an inbound peer update, it calls
/// @ref note_inbound() so the next poll that finds matching
/// content stays quiet.
///
/// Scope: pure detection — no peer fan-out, no size limit.
/// The orchestrator's outbound gate handles workspace
/// membership + max-text-size + cross-mesh forwarding.
///
/// Threading: owns one thread. start() / stop() are idempotent;
/// the destructor stops cleanly.
#pragma once

#include "orchestrator/clipboard_backend.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace unio_ui::orchestrator {

class ClipboardMonitor {
public:
    /// @brief Fired (from the polling thread) whenever the
    /// local clipboard changes to a value we haven't already
    /// observed via @ref note_inbound. Argument is the new text.
    using OnChangeFn = std::function<void(const std::string& content)>;

    /// @param backend  Borrowed pointer to the platform clipboard
    ///                 backend; the monitor doesn't own it.
    /// @param on_change Callback invoked on each detected change.
    ClipboardMonitor(IClipboardBackend* backend, OnChangeFn on_change);

    ~ClipboardMonitor();

    ClipboardMonitor(const ClipboardMonitor&)            = delete;
    ClipboardMonitor& operator=(const ClipboardMonitor&) = delete;

    /// @brief Spawn the polling thread. @p interval defaults to
    /// 500 ms (same cadence as the Python tree). Idempotent.
    void start(std::chrono::milliseconds interval =
               std::chrono::milliseconds(500));

    /// @brief Stop + join the polling thread. Safe from any thread;
    /// idempotent.
    void stop();

    /// @brief Tell the monitor that @p content was just written
    /// to the clipboard via an inbound peer update — the next
    /// poll that reads exactly this text will be silently
    /// dropped instead of re-broadcasting (would otherwise
    /// loop the same payload around the mesh).
    void note_inbound(const std::string& content);

private:
    void run_loop();

    IClipboardBackend*        backend_;
    OnChangeFn                on_change_;
    std::chrono::milliseconds interval_{500};
    std::thread               thread_;
    std::atomic<bool>         running_{false};

    std::mutex                m_;
    std::string               last_seen_;
    std::string               pending_echo_;
    bool                      have_pending_echo_ = false;
};

}  // namespace unio_ui::orchestrator
