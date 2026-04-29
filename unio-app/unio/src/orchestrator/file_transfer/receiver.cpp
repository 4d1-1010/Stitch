/// @file receiver.cpp
/// @brief Implementation of @ref FileTransferReceiver. Walks
/// the wire sequence into a per-transfer temp directory and,
/// on the trailing FileTransferEnd, hands the materialised
/// selection roots to the clipboard backend so the user's
/// file manager paste finds the files.

#include "orchestrator/file_transfer_receiver.hpp"

#include "orchestrator/clipboard_monitor.hpp"

#include <cstdio>
#include <ios>
#include <system_error>
#include <utility>

namespace unio_ui::orchestrator {

namespace fs = std::filesystem;

namespace {

/// @brief Stringify a u64 transfer_id as the temp-subdir name.
/// 16 hex chars — keeps directory names short and
/// case-insensitively unique across concurrent transfers.
std::string transfer_dir_name(std::uint64_t id) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(id));
    return buf;
}

/// @brief Reject relative paths that try to climb out of the
/// transfer dir (../ traversal). A misbehaving or hostile
/// peer could otherwise overwrite anything writable by our
/// process.
bool path_is_safe(const std::string& rel) {
    if (rel.empty()) return false;
    if (rel.front() == '/') return false;
    // Walk components looking for "..".
    fs::path p(rel);
    for (const auto& part : p) {
        const std::string s = part.string();
        if (s == "..") return false;
        if (s == "/")  return false;
    }
    return true;
}

}  // namespace

FileTransferReceiver::FileTransferReceiver(
    IClipboardBackend*    backend,
    ClipboardMonitor*     monitor,
    std::filesystem::path temp_root,
    OnPublishedFn         on_published)
    : backend_(backend),
      monitor_(monitor),
      temp_root_(std::move(temp_root)),
      on_published_(std::move(on_published)) {
    // Startup sweep — wipe every per-transfer subdir left over
    // from a prior session (graceful exit, crash, or SIGKILL all
    // can leave stale dirs). The OS clipboard can still hold a
    // selection pointing at one of these from before the
    // restart, but the source app for that paste is gone with
    // the previous unio-ui process anyway.
    std::error_code ec;
    if (fs::exists(temp_root_, ec)) {
        for (const auto& entry : fs::directory_iterator(temp_root_, ec)) {
            if (ec) break;
            remove_dir_tree(entry.path());
        }
    }
    // Re-create cleanly so on_start never hits "directory not
    // found" on an idle system.
    fs::create_directories(temp_root_, ec);
}

FileTransferReceiver::~FileTransferReceiver() {
    // Best-effort: tear down every still-active transfer's
    // temp dir AND the most recently published one. Crashes /
    // SIGKILL can't run this — the startup sweep above is the
    // durable cleanup path.
    std::lock_guard lk(m_);
    for (auto& [id, t] : active_) {
        if (!t) continue;
        for (auto& h : t->handles) if (h && h->is_open()) h->close();
        remove_dir_tree(t->dir);
    }
    active_.clear();
    if (!last_published_dir_.empty()) {
        remove_dir_tree(last_published_dir_);
        last_published_dir_.clear();
    }
}

void FileTransferReceiver::remove_dir_tree(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

void FileTransferReceiver::on_start(
    const std::string& peer,
    const control::FileTransferStartMessage& m) {
    auto t = std::make_unique<ActiveTransfer>();
    t->source_machine   = peer;
    t->selection_roots  = m.selection_roots;
    t->files            = m.files;
    t->handles.resize(m.files.size());
    t->bytes_per_file.assign(m.files.size(), 0);
    for (const auto& f : m.files) t->bytes_total += f.size;

    t->dir = temp_root_ / transfer_dir_name(m.transfer_id);
    // Replace any leftover directory from a (vanishingly
    // unlikely) prior transfer that re-rolled the same id.
    remove_dir_tree(t->dir);
    std::error_code ec;
    fs::create_directories(t->dir, ec);
    if (ec) {
        std::fprintf(stderr,
                     "file_xfer: create temp dir %s failed: %s\n",
                     t->dir.string().c_str(),
                     ec.message().c_str());
        return;
    }
    std::fprintf(stderr,
                 "file_xfer: rx start %llu from %s "
                 "(%zu files, %llu bytes)\n",
                 static_cast<unsigned long long>(m.transfer_id),
                 peer.c_str(),
                 m.files.size(),
                 static_cast<unsigned long long>(t->bytes_total));

    std::lock_guard lk(m_);
    active_[m.transfer_id] = std::move(t);
}

void FileTransferReceiver::on_chunk(
    const std::string& /*peer*/,
    const control::FileChunkMessage& m) {
    std::lock_guard lk(m_);
    auto it = active_.find(m.transfer_id);
    if (it == active_.end() || !it->second) return;
    auto& t = *it->second;
    if (m.file_index >= t.files.size()) return;
    if (!path_is_safe(t.files[m.file_index].relative_path)) {
        std::fprintf(stderr,
                     "file_xfer: rejecting unsafe path '%s'\n",
                     t.files[m.file_index].relative_path.c_str());
        return;
    }

    // Lazy-open the file handle on the first chunk for this
    // file index. Subdirectories in the relative_path are
    // created on the fly so an empty dir tree from
    // create_directories() at start time isn't required.
    auto& handle = t.handles[m.file_index];
    if (!handle) {
        const fs::path full =
            t.dir / t.files[m.file_index].relative_path;
        std::error_code ec;
        fs::create_directories(full.parent_path(), ec);
        handle = std::make_unique<std::ofstream>(
            full, std::ios::binary | std::ios::trunc);
        if (!handle->is_open()) {
            std::fprintf(stderr,
                         "file_xfer: open %s for write failed\n",
                         full.string().c_str());
            return;
        }
    }
    if (!m.data.empty()) {
        handle->write(reinterpret_cast<const char*>(m.data.data()),
                      static_cast<std::streamsize>(m.data.size()));
        t.bytes_received        += m.data.size();
        t.bytes_per_file[m.file_index] += m.data.size();
    }
    t.current_file_idx = m.file_index;
    if (m.is_last) {
        handle->close();
    }
}

void FileTransferReceiver::on_end(
    const std::string& /*peer*/,
    const control::FileTransferEndMessage& m) {
    std::unique_ptr<ActiveTransfer> t;
    {
        std::lock_guard lk(m_);
        auto it = active_.find(m.transfer_id);
        if (it == active_.end()) return;
        t = std::move(it->second);
        active_.erase(it);
    }
    // Make sure every file handle is closed (the trailing
    // FileChunk's is_last flag should have done it; this is
    // belt-and-braces).
    for (auto& h : t->handles) if (h && h->is_open()) h->close();
    std::fprintf(stderr,
                 "file_xfer: rx end %llu — publishing %zu "
                 "selection roots to clipboard\n",
                 static_cast<unsigned long long>(m.transfer_id),
                 t->selection_roots.size());
    {
        std::lock_guard lk(m_);
        publish_to_clipboard_locked(*t);
    }
    if (on_published_) on_published_();
}

void FileTransferReceiver::on_cancel(
    const std::string& peer,
    const control::FileTransferCancelMessage& m) {
    std::unique_ptr<ActiveTransfer> t;
    {
        std::lock_guard lk(m_);
        auto it = active_.find(m.transfer_id);
        if (it == active_.end()) return;
        t = std::move(it->second);
        active_.erase(it);
    }
    std::fprintf(stderr,
                 "file_xfer: rx cancel %llu from %s "
                 "(reason: %s)\n",
                 static_cast<unsigned long long>(m.transfer_id),
                 peer.c_str(),
                 m.reason.empty() ? "-" : m.reason.c_str());
    for (auto& h : t->handles) if (h && h->is_open()) h->close();
    remove_dir_tree(t->dir);
}

void FileTransferReceiver::publish_to_clipboard_locked(
    const ActiveTransfer& t) {
    if (backend_ == nullptr) return;
    // Map each top-level selection root the source advertised
    // to its absolute path under our temp dir. The user's file
    // manager paste sees the file or directory at that path
    // and copies it wherever the user is pasting.
    std::vector<std::string> abs_paths;
    abs_paths.reserve(t.selection_roots.size());
    for (const auto& root : t.selection_roots) {
        const fs::path p = t.dir / root;
        abs_paths.push_back(p.string());
    }
    backend_->set_clipboard_files(abs_paths);

    // Echo-suppression: after set_clipboard_files we own the
    // selection. The next clipboard-monitor poll will re-read
    // our own files; tell it to drop that read instead of
    // re-broadcasting it back to the original sender.
    if (monitor_ != nullptr) {
        const ClipboardFiles snapshot =
            backend_->get_clipboard_files();
        monitor_->note_inbound_files(snapshot);
    }

    // Roll the previous published dir. The OS clipboard now
    // points at @c t.dir, so any earlier transfer dir is no
    // longer reachable through the user's paste flow — safe
    // to delete without stranding a paste in progress.
    // (A paste already in flight against the old dir will have
    // opened those files by now; the unlink only removes the
    // directory entry.)
    if (!last_published_dir_.empty()
        && last_published_dir_ != t.dir) {
        remove_dir_tree(last_published_dir_);
    }
    last_published_dir_ = t.dir;
}

std::vector<FileTransferReceiver::Progress>
FileTransferReceiver::progress_snapshot() const {
    std::lock_guard lk(m_);
    std::vector<Progress> out;
    out.reserve(active_.size());
    for (const auto& [id, t] : active_) {
        if (!t) continue;
        Progress p;
        p.transfer_id      = id;
        p.source_machine   = t->source_machine;
        p.selection_roots  = t->selection_roots;
        p.bytes_received   = t->bytes_received;
        p.bytes_total      = t->bytes_total;
        p.current_file_idx = t->current_file_idx;
        p.file_count       =
            static_cast<std::uint32_t>(t->files.size());
        out.push_back(std::move(p));
    }
    return out;
}

}  // namespace unio_ui::orchestrator
