/// @file file_transfer_sender.hpp
/// @brief One-shot file-transfer sender. Spawns a background
/// thread that reads each file in 64 KB chunks and pushes
/// FileChunk frames over the control channel, fanning out to
/// every target peer in sequence. The orchestrator owns one
/// sender instance per active transfer (keyed by
/// @c transfer_id) and disposes of it once the @c on_finished
/// callback fires.
///
/// Threading: the sender thread is the only producer of
/// @ref Progress writes; any thread can read via @c progress()
/// (mutex-guarded snapshot). @c cancel() is also thread-safe.
///
/// Per-message flow on the wire: FileTransferStart (manifest)
/// → N × FileChunk (last chunk per file flagged is_last=true)
/// → FileTransferEnd. Receiver only commits the OS clipboard
/// after seeing FileTransferEnd, so a paste mid-transfer can't
/// race onto half-written files.
#pragma once

#include "orchestrator/clipboard/backend.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xorio::orchestrator {

namespace control { class IControlChannel; }

class FileTransferSender {
public:
    /// @brief Live progress snapshot. Read by the UI overlay
    /// every frame and by the orchestrator when deciding to
    /// dispose finished senders.
    struct Progress {
        std::uint64_t bytes_sent       = 0;
        std::uint64_t bytes_total      = 0;
        std::uint32_t current_file_idx = 0;
        std::uint32_t file_count       = 0;
        bool          done             = false;
        bool          cancelled        = false;
        bool          failed           = false;
    };

    /// @brief Fired exactly once when the sender finishes —
    /// either after the trailing FileTransferEnd reaches the
    /// channel, after a cancel acks out, or after a fatal
    /// read error. The orchestrator uses this to drop the
    /// sender from its tracking map.
    using OnFinishedFn = std::function<void(std::uint64_t transfer_id)>;

    /// @brief Construct + start the sender thread immediately.
    /// @param channel        Borrowed control channel — must
    ///                       outlive this sender.
    /// @param transfer_id    Unique random ID to disambiguate
    ///                       concurrent transfers.
    /// @param targets        Peer machine_ids that receive the
    ///                       transfer.
    /// @param files          Flat list of leaf files (folders
    ///                       already recursed into); each
    ///                       entry's @c absolute_path is read,
    ///                       @c relative_path advertised to
    ///                       the receiver.
    /// @param selection_roots Top-level selection names the
    ///                        receiver places on its OS
    ///                        clipboard.
    /// @param source_machine This peer's machine_id.
    /// @param on_finished    Fired once when the sender exits;
    ///                       safe to be empty.
    FileTransferSender(
        control::IControlChannel*    channel,
        std::uint64_t                transfer_id,
        std::vector<std::string>     targets,
        std::vector<LocalFile>       files,
        std::vector<std::string>     selection_roots,
        std::string                  source_machine,
        OnFinishedFn                 on_finished);

    ~FileTransferSender();

    FileTransferSender(const FileTransferSender&)            = delete;
    FileTransferSender& operator=(const FileTransferSender&) = delete;

    /// @brief Signal the sender thread to stop. Sends a
    /// FileTransferCancel to every target before exiting.
    /// Thread-safe and idempotent. The destructor calls this
    /// implicitly so an orchestrator drop is non-blocking.
    void cancel(std::string reason = {});

    Progress progress() const;

    std::uint64_t transfer_id() const { return transfer_id_; }

private:
    void run();

    control::IControlChannel*        channel_;
    std::uint64_t                    transfer_id_;
    std::vector<std::string>         targets_;
    std::vector<LocalFile>           files_;
    std::vector<std::string>         selection_roots_;
    std::string                      source_machine_;
    OnFinishedFn                     on_finished_;

    std::thread                      thread_;
    std::atomic<bool>                cancel_flag_{false};
    std::string                      cancel_reason_;
    std::mutex                       cancel_reason_m_;

    mutable std::mutex               progress_m_;
    Progress                         progress_;
};

}  // namespace xorio::orchestrator
