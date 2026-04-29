/// @file local_probe.hpp
/// @brief Adapter that queries this machine's display geometry
/// and surfaces it as a @ref CapsRecord.
#pragma once

#include "orchestrator/mesh_crdt.hpp"

#include <memory>

namespace unio_ui::orchestrator {

/// @brief Local-capability bridge.
///
/// Real platform impls live under `src/orchestrator/platform/`:
///   * `local_probe_x11.cpp`   — RandR enumeration on Linux/X11.
///   * `local_probe_win32.cpp` — `EnumDisplayMonitors` on Windows.
///
/// Encoder / decoder / presenter / capture-backend lists stay
/// empty for now; a future PR populates them from the in-process
/// `unio::pipe::Probe()` once the media-pipe layer is folded into
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
};

/// @brief Platform-aware factory. Returns a probe wired to the
/// host's real display enumeration; never returns nullptr — on a
/// host with zero detectable displays the probe surfaces an empty
/// `displays` vector instead.
std::unique_ptr<ILocalProbeAdapter> make_local_probe();

}  // namespace unio_ui::orchestrator
