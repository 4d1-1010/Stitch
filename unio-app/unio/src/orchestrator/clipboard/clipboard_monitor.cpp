/// @file clipboard_monitor.cpp
/// @brief Implementation of @ref ClipboardMonitor — single
/// thread, pumps the backend's event loop between reads.

#include "orchestrator/clipboard/monitor.hpp"

#include <utility>

namespace unio_ui::orchestrator {

ClipboardMonitor::ClipboardMonitor(IClipboardBackend* backend,
                                     OnChangeFn on_change,
                                     OnFilesFn  on_files)
    : backend_(backend),
      on_change_(std::move(on_change)),
      on_files_(std::move(on_files)) {}

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

void ClipboardMonitor::note_inbound_files(const ClipboardFiles& files) {
    std::lock_guard lk(m_);
    pending_echo_files_      = files;
    have_pending_echo_files_ = true;
    last_seen_files_         = files;
}

void ClipboardMonitor::run_loop() {
    // Seed both last_seen snapshots from the current clipboard
    // so the very first poll after startup doesn't broadcast
    // whatever the user happened to have copied before unio-ui
    // launched.
    if (backend_ != nullptr) {
        ClipboardData  initial_text  = backend_->get_clipboard();
        ClipboardFiles initial_files = backend_->get_clipboard_files();
        std::lock_guard lk(m_);
        last_seen_       = std::move(initial_text);
        last_seen_files_ = std::move(initial_files);
    }

    while (running_.load(std::memory_order_acquire)) {
        if (backend_ != nullptr) {
            // Text / HTML / image change-detection.
            const ClipboardData current = backend_->get_clipboard();
            {
                std::lock_guard lk(m_);
                if (current != last_seen_) {
                    last_seen_ = current;
                    if (have_pending_echo_
                        && current == pending_echo_) {
                        have_pending_echo_ = false;
                        pending_echo_      = {};
                    } else {
                        have_pending_echo_ = false;
                        pending_echo_      = {};
                        if (on_change_) on_change_(current);
                    }
                }
            }
            // File-selection change-detection. Independent
            // of text/html/image — apps usually put either
            // text on the clipboard or files (file managers
            // copy URI lists, document editors copy text),
            // so the two paths fire on different events.
            const ClipboardFiles current_files =
                backend_->get_clipboard_files();
            {
                std::lock_guard lk(m_);
                if (current_files != last_seen_files_) {
                    last_seen_files_ = current_files;
                    if (have_pending_echo_files_
                        && current_files == pending_echo_files_) {
                        have_pending_echo_files_ = false;
                        pending_echo_files_      = {};
                    } else {
                        have_pending_echo_files_ = false;
                        pending_echo_files_      = {};
                        if (on_files_ && !current_files.empty()) {
                            on_files_(current_files);
                        }
                    }
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
