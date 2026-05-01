/// @file local_probe.hpp
/// @brief Adapter that queries this machine's display geometry
/// and surfaces it as a @ref CapsRecord.
#pragma once

#include "orchestrator/mesh_crdt.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace xorio::orchestrator {

/// @brief Desired OS-local placement for a single monitor, used by
/// @ref ILocalProbeAdapter::apply_arrangement to drive the actual
/// system display arrangement from the workspace's saved layout.
struct DisplayPlacement {
    std::string  monitor_id;  ///< Matches @ref Display::monitor_id from probe().
    std::int32_t x = 0;       ///< OS-local x in px.
    std::int32_t y = 0;       ///< OS-local y in px.
};

/// @brief Local-capability bridge.
///
/// Real platform impls live under `src/orchestrator/platform/`:
///   * `local_probe_x11.cpp`   — RandR enumeration on Linux/X11.
///   * `local_probe_win32.cpp` — `EnumDisplayMonitors` on Windows.
///
/// Encoder / decoder / presenter / capture-backend lists stay
/// empty for now; a future PR populates them from the in-process
/// `xorio::pipe::Probe()` once the media-pipe layer is folded into
/// the same binary.
class ILocalProbeAdapter {
public:
    virtual ~ILocalProbeAdapter() = default;

    /// @brief Produce a fresh @ref CapsRecord for the local peer.
    ///
    /// Called on boot and whenever a hardware-change notification
    /// arrives. The orchestrator signs the result with its
    /// pairing key and publishes it to the @c caps CRDT slot.
    virtual CapsRecord probe() const = 0;

    /// @brief Set the OS-level display arrangement for the local
    /// monitors. Each placement maps a monitor_id (matching what
    /// @ref probe() returns) to its desired OS-local x,y.
    ///
    /// The orchestrator calls this when a workspace's layout entry
    /// for a local monitor moves, so the actual system display
    /// arrangement stays consistent with what the user dragged in
    /// the Layout tab. Default impl is a no-op for platforms that
    /// don't yet support OS-side rearrangement.
    virtual void apply_arrangement(
        const std::vector<DisplayPlacement>& /*placements*/) const {}
};

/// @brief Platform-aware factory. Returns a probe wired to the
/// host's real display enumeration; never returns nullptr — on a
/// host with zero detectable displays the probe surfaces an empty
/// `displays` vector instead.
std::unique_ptr<ILocalProbeAdapter> make_local_probe();

}  // namespace xorio::orchestrator
