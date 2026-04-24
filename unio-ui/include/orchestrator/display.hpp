/// @file display.hpp
/// @brief Physical display contributed by a peer.
#pragma once

#include <cstdint>
#include <string>

namespace unio_ui::orchestrator {

/// @brief A physical monitor known to the mesh.
///
/// Coordinates are in the owning peer's global-desktop space.
/// The mesh's @c caps CRDT record carries one @ref Display per
/// connector the peer's local probe reported.
struct Display {
    std::string  machine_id;   ///< Owning peer (see @ref Peer::machine_id).
    std::string  monitor_id;   ///< OS-level connector name (e.g. @c HDMI-1).
    std::int32_t global_x = 0; ///< Top-left X in the owner's global-desktop space.
    std::int32_t global_y = 0;
    std::int32_t width    = 0;
    std::int32_t height   = 0;
    std::int32_t number   = 0; ///< 1-based OS display ordinal.
};

}  // namespace unio_ui::orchestrator
