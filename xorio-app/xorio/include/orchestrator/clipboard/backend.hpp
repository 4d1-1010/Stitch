/// @file clipboard_backend.hpp
/// @brief Per-OS clipboard read/write abstraction. Plain UTF-8
/// text only for v1 — the workspace's "Include rich text/images"
/// + "Include files" toggles ride on the wire (LAN announce) but
/// are not yet honoured by this layer.
///
/// Threading: get_clipboard() / set_clipboard() are called
/// from the orchestrator's clipboard-monitor thread.
/// Implementations run their own internal event thread when
/// the platform needs one (X11: SelectionRequest dispatch
/// from other apps pasting from us); Win32 has no such
/// requirement.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xorio::orchestrator {

/// @brief One leaf file the user selected (or a leaf inside a
/// selected folder, after recursive walk). Carries both the
/// local absolute path the source needs to read bytes and the
/// receiver-relative path used to recreate the subfolder
/// structure under the destination temp dir.
struct LocalFile {
    /// @brief Local-OS absolute path. Source-side only — the
    /// receiver clears this on materialisation.
    std::string   absolute_path;
    /// @brief Path relative to the user's selection root,
    /// using forward slashes. Empty for a single-file
    /// selection (basename equals the selection root).
    std::string   relative_path;
    /// @brief File size in bytes. Pre-populated from stat() so
    /// the wire announcement can pre-allocate file size on the
    /// receiver before any chunks arrive.
    std::uint64_t size = 0;
};

/// @brief Snapshot of the file-shaped portion of the OS
/// clipboard. Empty when the clipboard doesn't carry files
/// (text-only selection, image-only selection, etc.).
struct ClipboardFiles {
    /// @brief Top-level basenames the user actually selected.
    /// For "/home/u/folder" + "/home/u/notes.txt": ["folder",
    /// "notes.txt"]. The receiver's OS clipboard ends up with
    /// paths that map to these names.
    std::vector<std::string> selection_roots;

    /// @brief Flat list of every leaf file inside the
    /// selection (folders recursed into). Ordering is
    /// deterministic (stable across polls of the same
    /// unchanged selection) so the monitor's change-detection
    /// can compare snapshots structurally.
    std::vector<LocalFile>   files;

    bool empty() const { return files.empty(); }

    friend bool operator==(const ClipboardFiles& a,
                            const ClipboardFiles& b) {
        if (a.selection_roots != b.selection_roots) return false;
        if (a.files.size() != b.files.size()) return false;
        for (std::size_t i = 0; i < a.files.size(); ++i) {
            const auto& fa = a.files[i];
            const auto& fb = b.files[i];
            if (fa.absolute_path != fb.absolute_path
                || fa.relative_path != fb.relative_path
                || fa.size          != fb.size) {
                return false;
            }
        }
        return true;
    }
    friend bool operator!=(const ClipboardFiles& a,
                            const ClipboardFiles& b) {
        return !(a == b);
    }
};

/// @brief Multi-format clipboard payload. Plain text is the
/// fallback every receiver can apply; HTML and PNG are
/// optional and empty when the source clipboard didn't carry
/// them. The "Include rich text/images" workspace toggle
/// gates whether HTML and image_png are forwarded across the
/// mesh — the local clipboard pipeline always reads + writes
/// every format it can.
struct ClipboardData {
    /// @brief UTF-8 plain text. Always populated when the
    /// clipboard holds anything renderable as text — the
    /// receiver applies this regardless of the rich-text
    /// gate.
    std::string text;

    /// @brief HTML fragment or full document. Sources include
    /// browsers, rich-text editors, IDE syntax-highlighted
    /// copies. Receivers paste this when the destination app
    /// understands HTML; otherwise apps fall back to @c text.
    std::string html;

    /// @brief Raw image bytes in whatever format the source
    /// clipboard offered — PNG, JPEG, BMP. The matching MIME
    /// type is carried in @ref image_mime so the receiver can
    /// re-advertise it (X11 selection target) or pick the
    /// right OS clipboard format (Win32 CF_PNG vs. inlined
    /// HTML data-URL).
    std::vector<std::uint8_t> image_bytes;

    /// @brief Standard image MIME for @ref image_bytes —
    /// "image/png", "image/jpeg", "image/bmp", or empty when
    /// no image is present.
    std::string               image_mime;

    bool empty() const {
        return text.empty() && html.empty() && image_bytes.empty();
    }

    friend bool operator==(const ClipboardData& a,
                            const ClipboardData& b) {
        return a.text        == b.text
            && a.html        == b.html
            && a.image_mime  == b.image_mime
            && a.image_bytes == b.image_bytes;
    }
    friend bool operator!=(const ClipboardData& a,
                            const ClipboardData& b) {
        return !(a == b);
    }
};

class IClipboardBackend {
public:
    virtual ~IClipboardBackend() = default;

    /// @brief Open the platform connection. Returns false on
    /// failure (X server unreachable, etc.); the caller falls
    /// back to a no-op clipboard pipeline.
    virtual bool open() = 0;

    /// @brief Tear down the platform connection.
    virtual void close() = 0;

    /// @brief Read the current clipboard. Empty @ref ClipboardData
    /// if the clipboard holds nothing we recognise (no text,
    /// no HTML, no PNG image) or the read times out.
    virtual ClipboardData get_clipboard() = 0;

    /// @brief Write @p data to the system clipboard, taking
    /// ownership so other apps can paste from us. Every
    /// non-empty format in @p data is advertised; empty
    /// formats are simply not offered to requestors.
    virtual void set_clipboard(const ClipboardData& data) = 0;

    /// @brief Read the file-shaped portion of the OS clipboard
    /// — files / folders the user copied in their file
    /// manager. Empty result if the clipboard holds no files
    /// (text/image selection, no clipboard owner, X selection
    /// owner refused). Folder selections are recursed into so
    /// every leaf file appears in @ref ClipboardFiles::files
    /// alongside its relative path under the user's selection
    /// root.
    virtual ClipboardFiles get_clipboard_files() = 0;

    /// @brief Take ownership of the clipboard with @p paths as
    /// the file URIs / CF_HDROP entries. Used by the
    /// file-transfer receiver after it materialises files in
    /// the per-transfer temp dir — once these go on the
    /// clipboard, the user's file manager paste can read them.
    /// Default implementation is a no-op for backends that
    /// don't ship file transfer yet; the X11 / Win32 impls
    /// override to drive their native clipboard formats.
    virtual void set_clipboard_files(
        const std::vector<std::string>& /*absolute_paths*/) {}
};

/// @brief Construct the default backend for the current platform.
std::unique_ptr<IClipboardBackend> make_default_clipboard_backend();

}  // namespace xorio::orchestrator
