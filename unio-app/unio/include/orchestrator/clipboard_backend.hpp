/// @file clipboard_backend.hpp
/// @brief Per-OS clipboard read/write abstraction. Plain UTF-8
/// text only for v1 — the workspace's "Include rich text/images"
/// + "Include files" toggles ride on the wire (LAN announce) but
/// are not yet honoured by this layer.
///
/// Threading: get_text() / set_text() are called from the
/// orchestrator's clipboard-monitor thread. Implementations
/// run their own internal event thread when the platform
/// needs one (X11: SelectionRequest dispatch from other apps
/// pasting from us); Win32 has no such requirement.
#pragma once

#include <memory>
#include <string>

namespace unio_ui::orchestrator {

class IClipboardBackend {
public:
    virtual ~IClipboardBackend() = default;

    /// @brief Open the platform connection. Returns false on
    /// failure (X server unreachable, etc.); the caller falls
    /// back to a no-op clipboard pipeline.
    virtual bool open() = 0;

    /// @brief Tear down the platform connection.
    virtual void close() = 0;

    /// @brief Read the current clipboard text. Returns empty
    /// string if the clipboard is empty, holds non-text data,
    /// or the read times out.
    virtual std::string get_text() = 0;

    /// @brief Write @p text to the system clipboard, taking
    /// ownership so other apps can paste from us.
    virtual void set_text(const std::string& text) = 0;
};

/// @brief Construct the default backend for the current platform.
std::unique_ptr<IClipboardBackend> make_default_clipboard_backend();

}  // namespace unio_ui::orchestrator
