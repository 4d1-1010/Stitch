/// @file coordinator.cpp
/// @brief Clipboard + file-transfer method bodies of
/// @ref FacadeOrchestrator. Owns:
///   * The local clipboard monitor + its echo-suppression hooks.
///   * The wire protocol for "I have fresh content" announces and
///     "give me your bytes" fetch replies.
///   * The Ctrl+V keystroke intercept that defers the local paste
///     until the inbound bytes arrive.
///   * The file-transfer sender / receiver overlay wiring.
///
/// Class declaration lives in @c facade.hpp; the rest of
/// FacadeOrchestrator's bodies are split between
/// @c orchestrator.cpp (composition + dispatcher + worker) and
/// @c cursor_handoff.cpp (cursor router glue).

#include "orchestrator/facade.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>
#include <utility>

namespace unio_ui::orchestrator::detail {

std::size_t FacadeOrchestrator::clipboard_max_bytes(ClipboardLimit lim) {
    switch (lim) {
        case ClipboardLimit::Kb100:     return 100  * 1024;
        case ClipboardLimit::Mb1:       return 1    * 1024 * 1024;
        case ClipboardLimit::Mb5:       return 5    * 1024 * 1024;
        case ClipboardLimit::Mb10:      return 10   * 1024 * 1024;
        case ClipboardLimit::Unlimited: return 0;
    }
    return 0;
}

FacadeOrchestrator::ClipboardPolicy
FacadeOrchestrator::current_clipboard_policy() const {
    ClipboardPolicy out;
    if (!workspaces_) return out;
    const auto wss = workspaces_->list();
    for (const auto& ws : wss) {
        if (ws.members.count(local_machine_id_) == 0) continue;
        out.ws_members       = ws.members;
        out.outbound_allowed =
            ws.clipboard_members.count(local_machine_id_) > 0;
        out.allow_rich       = ws.clipboard_rich;
        out.allow_files      = ws.clipboard_files;
        out.max_text_bytes   = clipboard_max_bytes(ws.clipboard_max);
        break;
    }
    return out;
}

std::string
FacadeOrchestrator::peer_display_name_for(const std::string& mid) const {
    std::lock_guard lk(peers_m_);
    auto it = peers_.find(mid);
    if (it == peers_.end() || it->second.display_name.empty()) {
        return mid;
    }
    return it->second.display_name;
}

std::string FacadeOrchestrator::summarize_roots(
        const std::vector<std::string>& roots) {
    if (roots.empty()) return "(no files)";
    if (roots.size() == 1) return roots[0];
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "%zu items", roots.size());
    return buf;
}

void FacadeOrchestrator::wire_clipboard_monitor() {
    if (!clipboard_backend_) return;
    clipboard_monitor_ = std::make_unique<ClipboardMonitor>(
        clipboard_backend_.get(),
        [this](const ClipboardData& data) {
            capture_local_clipboard(data);
        },
        [this](const ClipboardFiles& files) {
            capture_local_files(files);
        });
    clipboard_monitor_->start();
}

void FacadeOrchestrator::wire_transfer_overlay() {
    transfer_overlay_ = platform::make_transfer_overlay();
    if (!transfer_overlay_) return;
    transfer_overlay_->set_cancel_handler(
        [this](std::uint64_t transfer_id) {
            cancel_transfer(transfer_id);
        });
    transfer_overlay_->set_progress_fetcher(
        [this]() -> std::vector<platform::TransferOverlayItem> {
            std::vector<platform::TransferOverlayItem> out;

            // Outbound senders. Done / cancelled rows linger for
            // ~1 s before the on_finished cleanup erases them —
            // the overlay's 10 Hz poll would otherwise miss a
            // short-lived transfer entirely.
            {
                std::lock_guard lk(file_senders_m_);
                for (const auto& [id, s] : file_senders_) {
                    if (!s) continue;
                    const auto p = s->progress();
                    platform::TransferOverlayItem row;
                    row.direction        = platform::TransferOverlayItem
                                               ::Direction::Sending;
                    row.transfer_id      = id;
                    row.peer_name        = "peer";
                    row.label            = p.cancelled ? "cancelled"
                                          : p.failed   ? "failed"
                                          : p.done     ? "done"
                                          : "transfer";
                    row.bytes_done       = p.bytes_sent;
                    row.bytes_total      = p.bytes_total;
                    row.current_file_idx = p.current_file_idx;
                    row.file_count       = p.file_count;
                    out.push_back(std::move(row));
                }
            }

            // Inbound transfers.
            if (file_transfer_receiver_) {
                auto inbound = file_transfer_receiver_
                                 ->progress_snapshot();
                for (auto& p : inbound) {
                    platform::TransferOverlayItem row;
                    row.direction        = platform::TransferOverlayItem
                                               ::Direction::Receiving;
                    row.transfer_id      = p.transfer_id;
                    row.peer_name        = peer_display_name_for(
                                               p.source_machine);
                    row.label            = summarize_roots(
                                               p.selection_roots);
                    row.bytes_done       = p.bytes_received;
                    row.bytes_total      = p.bytes_total;
                    row.current_file_idx = p.current_file_idx;
                    row.file_count       = p.file_count;
                    out.push_back(std::move(row));
                }
            }
            return out;
        });
    transfer_overlay_->start();
}

void FacadeOrchestrator::wire_file_transfer_receiver() {
    if (!clipboard_backend_) return;
    // Stage on the same filesystem the user is most likely to
    // paste into (their home tree on Linux, profile drive on
    // Windows). The OS-side paste then degenerates to an O(1)
    // rename instead of a full copy — see
    // @ref publish_to_clipboard_locked, which sets the
    // clipboard's drop-effect to MOVE so the file manager *moves*
    // the staged files into the destination.
    std::error_code ec;
    std::filesystem::path root;
#if defined(_WIN32)
    // %TEMP% is on the user's local profile drive, same FS as
    // Documents / Desktop / Downloads in nearly every setup.
    // Cross-drive pastes still fall back to a copy (no way around
    // that without write-direct-to-dest).
    root = std::filesystem::temp_directory_path(ec);
    if (ec) root = std::filesystem::path(".");
#else
    // ~/.cache/unio-clipboard — same filesystem as ~/Desktop etc.
    // Beats /tmp (often tmpfs on a separate FS, which would force
    // the OS paste to do a copy + delete instead of a rename).
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    if (xdg && *xdg) {
        root = std::filesystem::path(xdg);
    } else if (home && *home) {
        root = std::filesystem::path(home) / ".cache";
    } else {
        root = std::filesystem::temp_directory_path(ec);
        if (ec) root = std::filesystem::path("/tmp");
    }
#endif
    root /= "unio-clipboard";
    file_transfer_receiver_ =
        std::make_unique<FileTransferReceiver>(
            clipboard_backend_.get(),
            clipboard_monitor_.get(),
            std::move(root),
            [this]() {
                // Files just landed on the OS clipboard; if the
                // user pressed Ctrl+V earlier and we deferred V
                // until the bytes arrived, synthesise the
                // keystroke now.
                release_pending_paste();
            });
}

void FacadeOrchestrator::capture_local_files(const ClipboardFiles& files) {
    if (files.empty()) return;
    const auto policy = current_clipboard_policy();
    // Disabled-clipboard peers don't advertise. The local
    // clipboard still works for same-PC paste — we just stay
    // silent on the wire so other peers won't try to fetch from
    // us.
    if (!policy.outbound_allowed) return;
    if (!policy.allow_files)     return;

    std::lock_guard lk(local_clip_m_);
    // File selection clears any cached text/image (mirrors the
    // X11 backend's "files + text are mutually exclusive" rule).
    local_text_      = {};
    local_files_     = files;
    ++local_last_copy_t_;
    announce_local_clipboard_locked(/*has_text=*/false,
                                      /*has_html=*/false,
                                      /*has_image=*/false,
                                      /*has_files=*/true);
}

void FacadeOrchestrator::capture_local_clipboard(const ClipboardData& data) {
    const auto policy = current_clipboard_policy();
    if (!policy.outbound_allowed) return;

    ClipboardData stored;
    // Plain-text gate: oversize text stays local-only and never
    // crosses the wire, even on a fetch reply.
    if (policy.max_text_bytes == 0
        || data.text.size() <= policy.max_text_bytes) {
        stored.text = data.text;
    } else {
        std::fprintf(stderr,
                     "clipboard: text %zu bytes exceeds "
                     "workspace limit %zu — text dropped\n",
                     data.text.size(), policy.max_text_bytes);
    }
    if (policy.allow_rich) {
        stored.html        = data.html;
        stored.image_mime  = data.image_mime;
        stored.image_bytes = data.image_bytes;
    }
    if (stored.empty()) return;

    std::lock_guard lk(local_clip_m_);
    // Text/image clears any cached file selection — we can't have
    // both as the "current" copy.
    local_files_ = {};
    local_text_  = std::move(stored);
    ++local_last_copy_t_;
    announce_local_clipboard_locked(
        /*has_text=*/!local_text_.text.empty(),
        /*has_html=*/!local_text_.html.empty(),
        /*has_image=*/!local_text_.image_bytes.empty(),
        /*has_files=*/false);
}

void FacadeOrchestrator::announce_local_clipboard_locked(bool has_text,
                                                          bool has_html,
                                                          bool has_image,
                                                          bool has_files) {
    if (!control_channel_) return;
    const auto policy = current_clipboard_policy();
    if (!policy.outbound_allowed) return;

    control::ClipboardLatestMessage m;
    m.source_machine = local_machine_id_;
    m.last_copy_t    = local_last_copy_t_;
    m.has_text       = has_text;
    m.has_html       = has_html;
    m.has_image      = has_image;
    m.has_files      = has_files;
    const auto body  = control::encode_clipboard_latest(m);

    std::vector<std::string> targets;
    {
        std::lock_guard lk(connected_peers_m_);
        targets.reserve(connected_peers_.size());
        for (const auto& mid : connected_peers_) {
            if (policy.ws_members.count(mid) == 0) continue;
            targets.push_back(mid);
        }
    }
    std::fprintf(stderr,
                 "clipboard: announce t=%llu "
                 "(text=%d html=%d image=%d files=%d) "
                 "→ %zu peers\n",
                 static_cast<unsigned long long>(m.last_copy_t),
                 has_text, has_html, has_image, has_files,
                 targets.size());
    for (const auto& mid : targets) {
        control_channel_->send(mid,
                                control::MessageType::ClipboardLatest,
                                body.data(), body.size());
    }
}

void FacadeOrchestrator::handle_clipboard_latest_inbound(
        const std::string& peer,
        const control::ClipboardLatestMessage& m) {
    const auto policy = current_clipboard_policy();
    if (policy.ws_members.count(peer) == 0) return;
    std::fprintf(stderr,
                 "clipboard: latest from %s t=%llu "
                 "(text=%d html=%d image=%d files=%d)\n",
                 peer.c_str(),
                 static_cast<unsigned long long>(m.last_copy_t),
                 m.has_text, m.has_html, m.has_image, m.has_files);
    {
        std::lock_guard lk(remote_clip_m_);
        // Always overwrite — most-recently-received peer wins,
        // matching the user's "last Ctrl+C across the workspace"
        // rule.
        latest_remote_source_ = peer;
        latest_remote_t_      = m.last_copy_t;
        latest_remote_flags_  = (m.has_text  ? 0x01 : 0)
                              | (m.has_html  ? 0x02 : 0)
                              | (m.has_image ? 0x04 : 0)
                              | (m.has_files ? 0x08 : 0);
    }
    // If we're currently the active cursor peer, pre-fetch
    // immediately. Reason: a user physically at THIS PC will type
    // Ctrl+V locally, not via the dormant peer's keyboard
    // forwarding. Local key events hit the OS-focused app with
    // the OLD clipboard before our intercept_ctrl_v ever runs
    // (intercept only fires on inbound forwarded key events) — so
    // unless the bytes are already on the OS clipboard at paste
    // time, the local paste reads stale content. Pre-fetching on
    // announce eliminates that race for the active peer. Dormant
    // peers don't pre-fetch: their forwarded Ctrl+V flows through
    // intercept_ctrl_v which fires the fetch + defers the V
    // injection until bytes land — the original "no speculative
    // pulls on a passing-through cursor" guarantee still holds
    // for that path.
    if (cursor_router_ && cursor_router_->is_local_active()) {
        maybe_fetch_clipboard("active-peer-announce");
    }
}

void FacadeOrchestrator::maybe_fetch_clipboard(const char* trigger) {
    if (!control_channel_) return;
    std::string source;
    std::uint64_t t = 0;
    {
        std::lock_guard lk(remote_clip_m_);
        if (latest_remote_source_.empty()) return;
        // File content uses MOVE-on-paste semantics so the staged
        // files are consumed on the first paste; a repeat Ctrl+V
        // on the same (source, t) needs a fresh fetch to re-
        // stage. Text and image stay on the OS clipboard and can
        // be pasted repeatedly, so we keep the dedupe for them.
        const bool has_files =
            (latest_remote_flags_ & 0x08) != 0;
        if (!has_files
            && latest_remote_source_ == last_fetched_source_
            && latest_remote_t_   == last_fetched_t_) {
            return;
        }
        source = latest_remote_source_;
        t      = latest_remote_t_;
        last_fetched_source_ = source;
        last_fetched_t_      = t;
    }
    const auto policy = current_clipboard_policy();
    if (policy.ws_members.count(source) == 0) return;

    control::ClipboardFetchMessage req;
    req.requester_machine    = local_machine_id_;
    req.expected_last_copy_t = t;
    const auto body = control::encode_clipboard_fetch(req);
    std::fprintf(stderr,
                 "clipboard: fetch %s t=%llu (trigger=%s)\n",
                 source.c_str(),
                 static_cast<unsigned long long>(t),
                 trigger);
    control_channel_->send(source,
                            control::MessageType::ClipboardFetch,
                            body.data(), body.size());
}

void FacadeOrchestrator::handle_clipboard_fetch_inbound(
        const std::string& requester,
        const control::ClipboardFetchMessage& m) {
    const auto policy = current_clipboard_policy();
    if (policy.ws_members.count(requester) == 0) return;
    if (!policy.outbound_allowed)              return;

    ClipboardData  text_snapshot;
    ClipboardFiles files_snapshot;
    std::uint64_t  current_t = 0;
    {
        std::lock_guard lk(local_clip_m_);
        current_t = local_last_copy_t_;
        text_snapshot  = local_text_;
        files_snapshot = local_files_;
    }
    if (m.expected_last_copy_t != current_t) {
        std::fprintf(stderr,
                     "clipboard: fetch from %s expected t=%llu "
                     "but current t=%llu — ignoring\n",
                     requester.c_str(),
                     static_cast<unsigned long long>(
                         m.expected_last_copy_t),
                     static_cast<unsigned long long>(current_t));
        return;
    }

    // Text / HTML / image reply (inline single message).
    if (!text_snapshot.empty()) {
        control::ClipboardUpdateMessage reply;
        reply.source_machine = local_machine_id_;
        reply.text           = text_snapshot.text;
        if (policy.allow_rich) {
            reply.html        = text_snapshot.html;
            reply.image_mime  = text_snapshot.image_mime;
            reply.image_bytes = text_snapshot.image_bytes;
        }
        const auto body = control::encode_clipboard(reply);
        std::fprintf(stderr,
                     "clipboard: fetch reply text=%zu html=%zu "
                     "image=%zu → %s\n",
                     reply.text.size(), reply.html.size(),
                     reply.image_bytes.size(),
                     requester.c_str());
        control_channel_->send(requester,
                                control::MessageType::ClipboardUpdate,
                                body.data(), body.size());
    }

    // File reply — kick off a sender targeting only the
    // requester. The existing FileTransferStart/Chunk/End sequence
    // is reused; the receiver's @ref FileTransferReceiver
    // materialises into /tmp/unio-clipboard/<id>/ and writes the
    // OS clipboard on End.
    if (!files_snapshot.empty() && policy.allow_files) {
        std::uint64_t transfer_id;
        {
            static std::mt19937_64 rng(
                std::random_device{}());
            static std::mutex      rng_m;
            std::lock_guard lk(rng_m);
            transfer_id = rng();
        }
        std::fprintf(stderr,
                     "file_xfer: fetch reply tx=%llu "
                     "(%zu files) → %s\n",
                     static_cast<unsigned long long>(transfer_id),
                     files_snapshot.files.size(),
                     requester.c_str());
        auto sender = std::make_unique<FileTransferSender>(
            data_channel_.get(),
            transfer_id,
            std::vector<std::string>{requester},
            files_snapshot.files,
            files_snapshot.selection_roots,
            local_machine_id_,
            [this](std::uint64_t id) {
                // Linger before erase so the overlay's 10 Hz
                // refresh catches short transfers and the user
                // sees a brief "done" / "cancelled" state instead
                // of the row vanishing.
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1200));
                std::lock_guard lk(file_senders_m_);
                file_senders_.erase(id);
            });
        std::lock_guard lk(file_senders_m_);
        file_senders_[transfer_id] = std::move(sender);
    }
}

void FacadeOrchestrator::cancel_transfer(std::uint64_t transfer_id) {
    std::fprintf(stderr,
                 "file_xfer: user cancel tx=%llu\n",
                 static_cast<unsigned long long>(transfer_id));
    // Outbound first.
    {
        std::lock_guard lk(file_senders_m_);
        auto it = file_senders_.find(transfer_id);
        if (it != file_senders_.end() && it->second) {
            it->second->cancel("user cancelled");
            return;
        }
    }
    // Inbound — find the source from the receiver's active set,
    // signal the source, drop our state.
    if (!file_transfer_receiver_ || !control_channel_) return;
    const auto active =
        file_transfer_receiver_->progress_snapshot();
    for (const auto& a : active) {
        if (a.transfer_id != transfer_id) continue;
        control::FileTransferCancelMessage cm;
        cm.transfer_id = transfer_id;
        cm.reason      = "user cancelled";
        const auto body = control::encode_file_cancel(cm);
        // File-transfer cancel rides the data channel (same
        // channel as the chunks it's stopping).
        if (data_channel_) {
            data_channel_->send(
                a.source_machine,
                control::MessageType::FileTransferCancel,
                body.data(), body.size());
        }
        file_transfer_receiver_->on_cancel(a.source_machine, cm);
        // Cancel implicitly kills any deferred Ctrl+V waiting on
        // this transfer's bytes — otherwise the V would never be
        // released.
        paste_pending_ = false;
        v_swallowed_   = false;
        paste_ctrl_was_held_for_inject_ = false;
        return;
    }
}

void FacadeOrchestrator::abort_pending_paste() {
    if (!paste_pending_) return;
    std::fprintf(stderr,
                 "clipboard: aborting pending paste (Esc)\n");
    paste_pending_                  = false;
    paste_ctrl_was_held_for_inject_ = false;
    if (!file_transfer_receiver_) return;
    const auto active =
        file_transfer_receiver_->progress_snapshot();
    for (const auto& a : active) {
        control::FileTransferCancelMessage cm;
        cm.transfer_id = a.transfer_id;
        cm.reason      = "user-cancelled paste";
        const auto body = control::encode_file_cancel(cm);
        if (data_channel_) {
            data_channel_->send(
                a.source_machine,
                control::MessageType::FileTransferCancel,
                body.data(), body.size());
        }
        // Tear down our receiver-side state without waiting for
        // the source to ack.
        file_transfer_receiver_->on_cancel(a.source_machine, cm);
    }
}

bool FacadeOrchestrator::intercept_ctrl_v(std::uint32_t scancode,
                                           bool pressed) {
    // Escape during a pending paste is the user's "abort" signal —
    // cancel + drop the swallowed V so the OS never sees the
    // half-pasted keystroke.
    if (scancode == kHidEscape && pressed && paste_pending_) {
        abort_pending_paste();
        v_swallowed_ = false;
        return true;
    }
    // Track Ctrl held state across both modifier keys.
    if (scancode == kHidLeftCtrl || scancode == kHidRightCtrl) {
        if (pressed) ++ctrl_held_count_;
        else if (ctrl_held_count_ > 0) --ctrl_held_count_;
        return false;
    }
    if (scancode != kHidV) return false;

    if (pressed) {
        const bool ctrl_held = ctrl_held_count_ > 0;
        if (!ctrl_held) return false;
        if (!cursor_router_
            || !cursor_router_->is_local_active()) {
            return false;
        }
        // A previous Ctrl+V is still in flight — swallow this one
        // too so the OS can't paste stale content from a fall-
        // through. The deferred injection in
        // @ref release_pending_paste fires exactly once when the
        // fetch reply lands.
        if (paste_pending_) {
            v_swallowed_ = true;
            return true;
        }
        // If our local clipboard is already on the freshest known
        // (source, t) tuple, no fetch needed — let the V down
        // flow through and paste from whatever's already on the
        // local clipboard.
        //
        // Exception: file content uses "cut" / MOVE drop-effect
        // semantics, so the staged files are consumed by the
        // first paste. A second Ctrl+V on the same (source, t)
        // tuple needs a fresh fetch to re-stage; otherwise the OS
        // would try to paste from a path that no longer exists.
        // Text and image content stay on the clipboard and can be
        // pasted repeatedly without refetching.
        std::string source;
        std::uint64_t t = 0;
        bool          has_files = false;
        {
            std::lock_guard lk(remote_clip_m_);
            if (latest_remote_source_.empty()) return false;
            has_files = (latest_remote_flags_ & 0x08) != 0;
            if (!has_files
                && latest_remote_source_ == last_fetched_source_
                && latest_remote_t_   == last_fetched_t_) {
                return false;
            }
            source = latest_remote_source_;
            t      = latest_remote_t_;
        }
        std::fprintf(stderr,
                     "clipboard: Ctrl+V intercept — "
                     "fetch %s t=%llu, deferring V\n",
                     source.c_str(),
                     static_cast<unsigned long long>(t));
        v_swallowed_       = true;
        paste_pending_     = true;
        paste_ctrl_was_held_for_inject_ = (ctrl_held_count_ > 0);
        maybe_fetch_clipboard("ctrl-v");
        return true;
    }

    // V keyup: drop if the corresponding keydown was swallowed.
    // The synthesised release fires from
    // @ref release_pending_paste so the OS sees a fully-formed
    // key sequence.
    if (v_swallowed_) {
        v_swallowed_ = false;
        return true;
    }
    return false;
}

void FacadeOrchestrator::release_pending_paste() {
    if (!paste_pending_ || !input_backend_) return;
    paste_pending_ = false;
    std::fprintf(stderr,
                 "clipboard: releasing deferred V keystroke\n");
    // Re-assert Ctrl if the user released it during the fetch —
    // otherwise V alone would just type 'v'.
    const bool need_ctrl =
        (ctrl_held_count_ == 0)
        && paste_ctrl_was_held_for_inject_;
    if (need_ctrl) {
        input_backend_->inject_key(kHidLeftCtrl, true);
    }
    input_backend_->inject_key(kHidV, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    input_backend_->inject_key(kHidV, false);
    if (need_ctrl) {
        input_backend_->inject_key(kHidLeftCtrl, false);
    }
}

void FacadeOrchestrator::handle_clipboard_inbound(
        const std::string& peer,
        const control::ClipboardUpdateMessage& m) {
    if (!clipboard_backend_) return;
    const auto policy = current_clipboard_policy();
    std::fprintf(stderr,
                 "clipboard: inbound from %s "
                 "(text=%zu, html=%zu, image=%zu mime=%s) "
                 "allow_rich=%d\n",
                 peer.c_str(),
                 m.text.size(), m.html.size(),
                 m.image_bytes.size(),
                 m.image_mime.empty() ? "-"
                                        : m.image_mime.c_str(),
                 policy.allow_rich ? 1 : 0);
    if (policy.ws_members.count(peer) == 0) {
        return;
    }

    ClipboardData data;
    if (policy.max_text_bytes == 0
        || m.text.size() <= policy.max_text_bytes) {
        data.text = m.text;
    } else {
        std::fprintf(stderr,
                     "clipboard: inbound text from %s exceeds "
                     "limit (%zu > %zu) — text dropped\n",
                     peer.c_str(), m.text.size(),
                     policy.max_text_bytes);
    }
    if (policy.allow_rich) {
        data.html        = m.html;
        data.image_mime  = m.image_mime;
        data.image_bytes = m.image_bytes;
    }
    if (data.empty()) return;

    if (clipboard_monitor_) {
        // Mark BEFORE the write so the next poll that reads this
        // exact payload doesn't re-broadcast.
        clipboard_monitor_->note_inbound(data);
    }
    clipboard_backend_->set_clipboard(data);
    release_pending_paste();
}

}  // namespace unio_ui::orchestrator::detail
