/// @file clipboard_monitor.cpp
/// @brief Implementation of @ref ClipboardMonitor — single
/// thread, pumps the backend's event loop between reads.

#include "orchestrator/clipboard_monitor.hpp"

#include <utility>

namespace unio_ui::orchestrator {

ClipboardMonitor::ClipboardMonitor(IClipboardBackend* backend,
                                     OnChangeFn on_change)
    : backend_(backend),
      on_change_(std::move(on_change)) {}

ClipboardMonitor::~ClipboardMonitor() {
    stop();
}

void ClipboardMonitor::start(std::chrono::milliseconds interval) {
    if (running_.load(std::memory_order_acquire)) return;
    interval_ = interval;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ClipboardMonitor::run_loop, this);
}

void ClipboardMonitor::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

void ClipboardMonitor::note_inbound(const ClipboardData& data) {
    std::lock_guard lk(m_);
    pending_echo_      = data;
    have_pending_echo_ = true;
    last_seen_         = data;
}

void ClipboardMonitor::run_loop() {
    // Seed @ref last_seen_ from the current clipboard so the very
    // first poll after startup doesn't broadcast whatever the user
    // happened to have copied before unio-ui launched.
    if (backend_ != nullptr) {
        ClipboardData initial = backend_->get_clipboard();
        std::lock_guard lk(m_);
        last_seen_ = std::move(initial);
    }

    while (running_.load(std::memory_order_acquire)) {
        if (backend_ != nullptr) {
            const ClipboardData current = backend_->get_clipboard();
            std::lock_guard lk(m_);
            if (current != last_seen_) {
                last_seen_ = current;
                if (have_pending_echo_ && current == pending_echo_) {
                    // Just-injected inbound update — don't
                    // bounce it back across the mesh.
                    have_pending_echo_ = false;
                    pending_echo_      = {};
                } else {
                    have_pending_echo_ = false;
                    pending_echo_      = {};
                    if (on_change_) on_change_(current);
                }
            }
        }
        // Cadence is independent of the platform event pump —
        // the X11 backend runs its own event thread for paste
        // requests, so a sleep here doesn't stall outgoing
        // SelectionNotify replies.
        std::this_thread::sleep_for(interval_);
    }
}

}  // namespace unio_ui::orchestrator
