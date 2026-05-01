/// @file identify_overlay.hpp
/// @brief Per-monitor fullscreen overlay used by the Layout tab's
/// Identify button.
///
/// Scope: a single function that, given a list of monitor
/// rectangles + a number / label per monitor, paints a borderless
/// fullscreen rectangle on each one with the number + label
/// centred. Auto-dismisses after `duration_ms`.
///
/// Per-OS implementations live under
/// `src/platform/{x11,win32}/identify_overlay_*.cpp` and never
/// share state with the main UI's window — they own their own
/// display connection / message loop on a background thread so
/// the UI's render loop is unaffected.
///
/// Local-only today. Triggering overlays on remote peers needs
/// the mesh control-channel to fan the request out, which lands
/// when the channel itself does.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xorio::platform {

/// @brief One physical monitor's overlay request.
struct IdentifyOverlay {
    std::int32_t x      = 0;   ///< Top-left global-desktop X (px).
    std::int32_t y      = 0;   ///< Top-left global-desktop Y (px).
    std::int32_t width  = 0;
    std::int32_t height = 0;
    std::int32_t number = 0;   ///< Mesh-wide unique display number.
    std::string  label;        ///< Small caption — usually the machine_id.
};

/// @brief Fire one overlay window per @p overlays entry.
/// Non-blocking — returns immediately; the windows live until
/// @p duration_ms elapses, then are destroyed automatically.
/// Calling again before the previous run finished tears down the
/// in-flight overlays first.
void show_identify_overlays(const std::vector<IdentifyOverlay>& overlays,
                            int duration_ms);

}  // namespace xorio::platform
