/// @file local_probe.hpp
/// @brief Adapter that queries @c unio-pipe for this machine's
/// capabilities and surfaces them as a @ref CapsRecord.
#pragma once

#include "orchestrator/mesh_crdt.hpp"

namespace unio_ui::orchestrator {

/// @brief Local-capability bridge.
///
/// The real implementation calls @c unio::pipe::Probe() across an
/// in-process boundary (same binary once Phase 0 folds the layers
/// together). The mock returns a fixed record populated from a
/// compile-time snapshot of the dev machines.
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

}  // namespace unio_ui::orchestrator
