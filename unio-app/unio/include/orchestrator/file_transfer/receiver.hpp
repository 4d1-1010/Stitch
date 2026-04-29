/// @file file_transfer_receiver.hpp
/// @brief Inbound file-transfer assembly. Consumes the wire
/// sequence FileTransferStart → N × FileChunk →
/// FileTransferEnd, materialises every file under a
/// per-transfer temp directory, and on completion publishes
/// the selection roots to the OS clipboard via the backend
/// — at which point the user's file manager paste finds them.
///
/// Threading: every method runs on the control-channel reader
/// thread (the orchestrator dispatches the four file-transfer
/// message types directly into us). State is guarded by an
/// internal mutex so a future progress-snapshot reader can
/// peek concurrently from the UI thread.
///
/// One temp directory per transfer keeps concurrent transfers
/// — a user copying a second selection mid-stream — from
/// stomping each other. Cleanup of stale temp dirs from
/// previous unio-ui sessions is the orchestrator's job at
/// startup; this class only handles the lifecycle of the
/// transfers it sees.
#pragma once

#include "orchestrator/clipboard/backend.hpp"
#include "orchestrator/control/protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace unio_ui::orchestrator {

class ClipboardMonitor;

class FileTransferReceiver {
public:
    /// @brief Snapshot of one in-flight inbound transfer.
    /// Read by the UI overlay; bytes_total is the manifest
    /// total, bytes_received is what's hit disk so far.
    struct Progress {
        std::uint64_t            transfer_id      = 0;
        std::string              source_machine;
        std::vector<std::string> selection_roots;
        std::uint64_t            bytes_received   = 0;
        std::uint64_t            bytes_total      = 0;
        std::uint32_t            current_file_idx = 0;
        std::uint32_t            file_count       = 0;
    };

    /// @brief Fired exactly once per transfer, after on_end has
    /// materialised the files and published them to the OS
    /// clipboard. The orchestrator uses this to release a
    /// deferred Ctrl+V keystroke — once the clipboard holds
    /// the inbound selection, we can safely synthesise the
    /// paste keystroke the user pressed earlier.
    using OnPublishedFn = std::function<void()>;

    /// @param backend    Clipboard backend; the receiver calls
    ///                   set_clipboard_files() on completion.
    /// @param monitor    Clipboard monitor; the receiver calls
    ///                   note_inbound_files() so the next poll
    ///                   doesn't re-broadcast our own freshly-
    ///                   materialised selection.
    /// @param temp_root  Parent directory for per-transfer
    ///                   subdirs (e.g. /tmp/unio-clipboard).
    /// @param on_published Optional hook fired after a transfer
    ///                     ends + publishes its files to the
    ///                     OS clipboard. Safe to leave empty.
    FileTransferReceiver(IClipboardBackend*       backend,
                          ClipboardMonitor*        monitor,
                          std::filesystem::path    temp_root,
                          OnPublishedFn            on_published = {});

    ~FileTransferReceiver();

    FileTransferReceiver(const FileTransferReceiver&)            = delete;
    FileTransferReceiver& operator=(const FileTransferReceiver&) = delete;

    void on_start (const std::string& peer,
                    const control::FileTransferStartMessage&);
    void on_chunk (const std::string& peer,
                    const control::FileChunkMessage&);
    void on_end   (const std::string& peer,
                    const control::FileTransferEndMessage&);
    void on_cancel(const std::string& peer,
                    const control::FileTransferCancelMessage&);

    std::vector<Progress> progress_snapshot() const;

private:
    /// @brief Per-transfer state. One file handle per file in
    /// the manifest, opened lazily on the first chunk for
    /// that index so we don't blow through fd limits when a
    /// transfer carries thousands of files.
    struct ActiveTransfer {
        std::string                                    source_machine;
        std::vector<std::string>                       selection_roots;
        std::vector<control::FileTransferEntry>        files;
        std::vector<std::unique_ptr<std::ofstream>>    handles;
        std::vector<std::uint64_t>                     bytes_per_file;
        std::filesystem::path                          dir;
        std::uint64_t                                  bytes_received = 0;
        std::uint64_t                                  bytes_total    = 0;
        std::uint32_t                                  current_file_idx = 0;
    };

    /// @brief Recursive-rmdir helper; tolerates partially-
    /// torn-down trees.
    static void remove_dir_tree(const std::filesystem::path& p);

    /// @brief After FileTransferEnd lands, build the absolute
    /// paths the user's file manager will see when pasting,
    /// hand them to the backend, and prime the monitor's
    /// echo-suppression so the next clipboard poll doesn't
    /// re-broadcast our own output back to the source.
    void publish_to_clipboard_locked(
        const ActiveTransfer& t);

    IClipboardBackend*                                          backend_;
    ClipboardMonitor*                                           monitor_;
    std::filesystem::path                                       temp_root_;
    OnPublishedFn                                               on_published_;

    mutable std::mutex                                          m_;
    std::unordered_map<std::uint64_t,
                       std::unique_ptr<ActiveTransfer>>         active_;
    /// @brief Path of the per-transfer dir that the OS clipboard
    /// is currently pointing at (set by the most recent
    /// successful @ref publish_to_clipboard_locked). When the
    /// next transfer publishes, this dir is unlinked so we
    /// don't accumulate stale temp trees over the session.
    std::filesystem::path                                       last_published_dir_;
};

}  // namespace unio_ui::orchestrator
