/// @file source_overlay.hpp
/// @brief Source-side overlay that masks a monitor currently projected elsewhere.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace xorio_ui::platform {

/// @brief Placement + labelling for a @ref SourceOverlay.
struct SourceOverlayConfig {
    std::int32_t x      = 0;   ///< Global-desktop X of the source monitor.
    std::int32_t y      = 0;   ///< Global-desktop Y of the source monitor.
    std::int32_t width  = 0;
    std::int32_t height = 0;
    std::string source_label;      ///< Human caption, e.g. "adi-pc:eDP-1".
    std::string destination_label; ///< Remote target, e.g. "diana:DISPLAY1".
};

/// @brief Always-on-top, input-absorbing overlay that covers the
/// source monitor for the duration of an outgoing stream.
///
/// The window sets a capture-exclusion flag so the streamer does
/// not capture its own overlay (WDA_EXCLUDEFROMCAPTURE on Windows;
/// a custom XORIO_CAPTURE_EXCLUDE atom on X11).
class SourceOverlay {
public:
    virtual ~SourceOverlay() = default;
};

/// @brief Open a source-side overlay on the given monitor rectangle.
/// @return Owning handle; @c nullptr if the window could not be created.
std::unique_ptr<SourceOverlay>
open_source_overlay(const SourceOverlayConfig& cfg);

}  // namespace xorio_ui::platform
