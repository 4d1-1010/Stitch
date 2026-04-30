/// @file sender.cpp
/// @brief Implementation of @ref FileTransferSender. The
/// sender thread streams each file's bytes in
/// @ref kFileChunkSize-byte slices over the control channel
/// to every target peer. Send order: FileTransferStart →
/// N × FileChunk → FileTransferEnd. A cancel turns the
/// sequence into FileTransferCancel and exits early.

#include "orchestrator/file_transfer/sender.hpp"

#include "orchestrator/control/control_channel.hpp"
#include "orchestrator/control/protocol.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <ios>
#include <utility>

namespace xorio_ui::orchestrator {

FileTransferSender::FileTransferSender(
    control::IControlChannel*    channel,
    std::uint64_t                transfer_id,
    std::vector<std::string>     targets,
    std::vector<LocalFile>       files,
    std::vector<std::string>     selection_roots,
    std::string                  source_machine,
    OnFinishedFn                 on_finished)
    : channel_(channel),
      transfer_id_(transfer_id),
      targets_(std::move(targets)),
      files_(std::move(files)),
      selection_roots_(std::move(selection_roots)),
      source_machine_(std::move(source_machine)),
      on_finished_(std::move(on_finished)) {
    // Pre-fill total bytes so the overlay can display a
    // meaningful percentage from frame 0.
    std::uint64_t total = 0;
    for (const auto& f : files_) total += f.size;
    {
        std::lock_guard lk(progress_m_);
        progress_.bytes_total      = total;
        progress_.file_count       =
            static_cast<std::uint32_t>(files_.size());
        progress_.current_file_idx = 0;
    }
    thread_ = std::thread(&FileTransferSender::run, this);
}

FileTransferSender::~FileTransferSender() {
    cancel("sender destroyed");
    if (thread_.joinable()) thread_.join();
}

void FileTransferSender::cancel(std::string reason) {
    {
        std::lock_guard lk(cancel_reason_m_);
        if (cancel_reason_.empty() && !reason.empty()) {
            cancel_reason_ = std::move(reason);
        }
    }
    cancel_flag_.store(true, std::memory_order_release);
}

FileTransferSender::Progress
FileTransferSender::progress() const {
    std::lock_guard lk(progress_m_);
    return progress_;
}

void FileTransferSender::run() {
    // Helper: broadcast an already-encoded body to every
    // target peer. Inlined here (rather than a member
    // function) so the cpp doesn't need a separate template
    // declaration in the header.
    const auto broadcast =
        [this](control::MessageType type,
                const std::vector<std::uint8_t>& body) {
            if (channel_ == nullptr) return;
            for (const auto& peer : targets_) {
                channel_->send(peer, type,
                                body.data(), body.size());
            }
        };

    // 1) Manifest. Receiver pre-creates files at their final
    //    sizes so chunks can be written sequentially without
    //    seeking.
    {
        control::FileTransferStartMessage start;
        start.transfer_id     = transfer_id_;
        start.source_machine  = source_machine_;
        start.selection_roots = selection_roots_;
        start.files.reserve(files_.size());
        for (const auto& f : files_) {
            control::FileTransferEntry e;
            e.relative_path = f.relative_path;
            e.size          = f.size;
            start.files.push_back(std::move(e));
        }
        broadcast(control::MessageType::FileTransferStart,
                   control::encode_file_start(start));
    }

    // 2) Stream each file's bytes.
    std::vector<std::uint8_t> buf(control::kFileChunkSize);
    bool failed = false;
    for (std::uint32_t i = 0; i < files_.size(); ++i) {
        if (cancel_flag_.load(std::memory_order_acquire)) break;

        {
            std::lock_guard lk(progress_m_);
            progress_.current_file_idx = i;
        }

        std::ifstream f(files_[i].absolute_path,
                        std::ios::binary);
        if (!f) {
            std::fprintf(stderr,
                         "file_xfer: open %s failed; skipping\n",
                         files_[i].absolute_path.c_str());
            // Advance bytes_sent past the skipped file so
            // the overall percentage doesn't stall — better
            // UX than a transfer that visibly hangs at, say,
            // 23 % when one file out of many couldn't open.
            std::lock_guard lk(progress_m_);
            progress_.bytes_sent += files_[i].size;
            failed = true;
            continue;
        }

        std::uint64_t sent = 0;
        while (sent < files_[i].size
               && !cancel_flag_.load(std::memory_order_acquire)) {
            const std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(buf.size(),
                                        files_[i].size - sent));
            f.read(reinterpret_cast<char*>(buf.data()),
                   static_cast<std::streamsize>(want));
            const std::size_t got =
                static_cast<std::size_t>(f.gcount());
            if (got == 0) {
                // Source file truncated mid-read (size
                // changed since stat) — finish this file
                // early and move on.
                break;
            }

            control::FileChunkMessage chunk;
            chunk.transfer_id = transfer_id_;
            chunk.file_index  = i;
            chunk.is_last     =
                (sent + got >= files_[i].size);
            chunk.data.assign(buf.begin(),
                              buf.begin() + got);
            broadcast(control::MessageType::FileChunk,
                       control::encode_file_chunk(chunk));

            sent += got;
            {
                std::lock_guard lk(progress_m_);
                progress_.bytes_sent += got;
            }
            // Yield the per-peer send mutex between chunks so
            // cursor + keystroke frames (which share the same
            // TCP control channel) don't queue behind a long
            // run of file bytes. Without this, a multi-MB
            // transfer noticeably stalls the remote cursor while
            // the sender thread keeps re-acquiring the mutex.
            // 500 µs is enough for one cursor frame to slip
            // through and adds <5 % overhead on gigabit.
            std::this_thread::sleep_for(
                std::chrono::microseconds(500));
        }
    }

    // 3) Finalisation.
    const bool cancelled =
        cancel_flag_.load(std::memory_order_acquire);
    if (cancelled) {
        control::FileTransferCancelMessage cm;
        cm.transfer_id = transfer_id_;
        {
            std::lock_guard lk(cancel_reason_m_);
            cm.reason = cancel_reason_;
        }
        broadcast(control::MessageType::FileTransferCancel,
                   control::encode_file_cancel(cm));
    } else {
        control::FileTransferEndMessage em;
        em.transfer_id = transfer_id_;
        broadcast(control::MessageType::FileTransferEnd,
                   control::encode_file_end(em));
    }

    {
        std::lock_guard lk(progress_m_);
        progress_.done      = true;
        progress_.cancelled = cancelled;
        progress_.failed    = failed;
    }
    // @c on_finished_ is the orchestrator's "drop me from the
    // active-senders map" hook. Erasing the unique_ptr from
    // there triggers @c ~FileTransferSender, whose destructor
    // would then @c join() the very thread we're running in —
    // self-join throws @c std::system_error("Resource deadlock
    // avoided"). Detach first so the destructor sees a
    // non-joinable handle; the run function is about to return
    // anyway so there's nothing left to track.
    if (thread_.joinable()) thread_.detach();
    if (on_finished_) on_finished_(transfer_id_);
}

}  // namespace xorio_ui::orchestrator
