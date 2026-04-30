/// @file stream_window.hpp
/// @brief Borderless sink overlay that presents frames from a remote peer.
#pragma once

#include "imgui.h"

#include <memory>
#include <string>

namespace xorio_ui::platform {

/// @brief Placement + labelling for a @ref StreamWindow.
struct StreamWindowConfig {
    std::int32_t x      = 0;   ///< Global-desktop X of the target monitor.
    std::int32_t y      = 0;   ///< Global-desktop Y of the target monitor.
    std::int32_t width  = 0;   ///< Target rectangle width in pixels.
    std::int32_t height = 0;   ///< Target rectangle height in pixels.
    std::string  source_label; ///< Human caption, e.g. "adi-pc:HDMI-1".
};

/// @brief Fullscreen, borderless sink window that renders frames
/// produced by a remote @c unio-pipe decoder.
///
/// One instance per active inbound route. The owner (orchestrator)
/// keeps the object alive for the duration of the stream and
/// destroys it to close the window.
class StreamWindow {
public:
    virtual ~StreamWindow() = default;

    /// @brief Swap the next-to-present frame.
    /// @param texture  Backend-specific handle delivered by the
    ///                 presenter, cast to @c ImTextureID.
    ///
    /// @todo Wire real frame presentation when @c unio-pipe exposes
    /// its presenter texture through the orchestrator layer.
    virtual void push_frame(ImTextureID texture) = 0;
};

/// @brief Open a sink overlay on the target monitor rectangle.
/// @return Owning handle; @c nullptr if the window could not be created.
std::unique_ptr<StreamWindow>
open_stream_window(const StreamWindowConfig& cfg);

}  // namespace xorio_ui::platform
