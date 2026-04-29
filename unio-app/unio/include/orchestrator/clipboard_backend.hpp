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

namespace unio_ui::orchestrator {

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
};

/// @brief Construct the default backend for the current platform.
std::unique_ptr<IClipboardBackend> make_default_clipboard_backend();

}  // namespace unio_ui::orchestrator
