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

#include "orchestrator/clipboard/backend.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace xorio_ui::orchestrator {

class ClipboardMonitor {
public:
    /// @brief Fired (from the polling thread) whenever the
    /// local clipboard changes to a value we haven't already
    /// observed via @ref note_inbound. Argument is the new
    /// multi-format clipboard payload.
    using OnChangeFn = std::function<void(const ClipboardData& data)>;

    /// @brief Fired (from the polling thread) whenever the
    /// file-shaped portion of the clipboard changes (user
    /// copied / cut a different file selection in their file
    /// manager). The orchestrator dispatches a file-transfer
    /// flow in response. Echo-suppressed via
    /// @ref note_inbound_files so a freshly-materialised
    /// inbound transfer doesn't bounce back across the mesh.
    using OnFilesFn  = std::function<void(const ClipboardFiles& files)>;

    /// @param backend     Borrowed pointer to the platform
    ///                    clipboard backend; the monitor
    ///                    doesn't own it.
    /// @param on_change   Callback invoked on each detected
    ///                    text/HTML/image change.
    /// @param on_files    Callback invoked on each detected
    ///                    file-selection change. Empty
    ///                    function is allowed (useful for
    ///                    deployments that haven't wired the
    ///                    file-transfer engine yet).
    ClipboardMonitor(IClipboardBackend* backend,
                      OnChangeFn         on_change,
                      OnFilesFn          on_files = {});

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

    /// @brief Tell the monitor that @p data was just written to
    /// the clipboard via an inbound peer update — the next poll
    /// that reads exactly this payload (any non-empty subset of
    /// the same fields) will be silently dropped instead of
    /// re-broadcasting (would otherwise loop the same payload
    /// around the mesh).
    void note_inbound(const ClipboardData& data);

    /// @brief Tell the monitor that @p files was just written
    /// to the clipboard via an inbound file-transfer
    /// finalisation. Same echo-suppression guarantee as
    /// @ref note_inbound but for the file-shaped portion.
    void note_inbound_files(const ClipboardFiles& files);

private:
    void run_loop();

    IClipboardBackend*        backend_;
    OnChangeFn                on_change_;
    OnFilesFn                 on_files_;
    std::chrono::milliseconds interval_{500};
    std::thread               thread_;
    std::atomic<bool>         running_{false};

    std::mutex                m_;
    ClipboardData             last_seen_;
    ClipboardData             pending_echo_;
    bool                      have_pending_echo_ = false;

    ClipboardFiles            last_seen_files_;
    ClipboardFiles            pending_echo_files_;
    bool                      have_pending_echo_files_ = false;
};

}  // namespace xorio_ui::orchestrator
