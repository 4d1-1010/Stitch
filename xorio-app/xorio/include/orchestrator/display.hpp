/// @file display.hpp
/// @brief Physical display contributed by a peer.
#pragma once

#include <cstdint>
#include <string>

namespace xorio::orchestrator {

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

    friend bool operator==(const Display& a, const Display& b) {
        return a.machine_id == b.machine_id
            && a.monitor_id == b.monitor_id
            && a.global_x   == b.global_x
            && a.global_y   == b.global_y
            && a.width      == b.width
            && a.height     == b.height
            && a.number     == b.number;
    }
    friend bool operator!=(const Display& a, const Display& b) {
        return !(a == b);
    }
};

}  // namespace xorio::orchestrator
