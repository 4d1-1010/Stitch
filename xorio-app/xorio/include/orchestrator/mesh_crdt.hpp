/// @file mesh_crdt.hpp
/// @brief LWW CRDT records + interface for the mesh-sync layer.
#pragma once

#include "orchestrator/crypto/crypto.hpp"
#include "orchestrator/display.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace xorio::orchestrator {

/// @brief Capability snapshot published by each peer.
///
/// Produced by the local probe adapter; replicated by the mesh
/// CRDT under the peer's @c caps slot.
struct CapsRecord {
    std::string            machine_id;
    std::string            display_name;
    std::vector<Display>   displays;
    std::vector<std::string> encoders;     ///< e.g. "nvenc", "vaapi".
    std::vector<std::string> decoders;
    std::vector<std::string> presenters;
    std::vector<std::string> capture_backends;
    std::uint32_t          schema_version = 1;
};

/// @brief One routing line within a layout: "source display drives sink display".
struct RoutingLine {
    std::string src_machine_id;
    std::string src_monitor_id;
    std::string dst_machine_id;
    std::string dst_monitor_id;
};

/// @brief User-edited layout state published to every peer.
struct LayoutRecord {
    std::vector<RoutingLine> lines;
    std::uint32_t            schema_version = 1;
};

/// @brief Named workspace preset (saved @ref LayoutRecord snapshots).
struct WorkspaceRecord {
    std::string              id;       ///< UUID.
    std::string              name;
    std::vector<RoutingLine> lines;
    bool                     locked = false;
};

/// @brief Collection of all defined workspaces on one peer.
struct WorkspacesRecord {
    std::vector<WorkspaceRecord> workspaces;
    std::uint32_t                schema_version = 1;
};

/// @brief Shared clipboard payload.
struct ClipboardRecord {
    std::string                  mime_type;
    std::vector<std::uint8_t>    data;
    std::uint32_t                schema_version = 1;
};

/// @brief Heartbeat stamp surfaced as a CRDT slot for presence.
struct PresenceRecord {
    std::uint64_t last_seen_ns = 0;  ///< steady_clock nanoseconds since epoch.
};

/// @brief Pairing record — stored per bilateral trust relationship.
struct PairingRecord {
    crypto::PairingPublicKey peer_public_key;
    std::string              peer_machine_id;
    std::uint64_t            established_ns = 0;
};

/// @brief The CRDT slots tracked per peer.
enum class Slot {
    Caps,
    Layout,
    Workspaces,
    Clipboard,
    Presence,
    Pairing,
};

/// @brief Mesh-sync interface.
///
/// Each peer holds one instance. Reads return merged-latest state
/// across all paired peers; writes apply locally + schedule a
/// gossip round on the control-connection CRDT stream.
class IMeshCrdt {
public:
    virtual ~IMeshCrdt() = default;

    /// @name Local writes
    /// @{
    virtual void put_caps(const CapsRecord& rec) = 0;
    virtual void put_layout(const LayoutRecord& rec) = 0;
    virtual void put_workspaces(const WorkspacesRecord& rec) = 0;
    virtual void put_clipboard(const ClipboardRecord& rec) = 0;
    virtual void put_presence(const PresenceRecord& rec) = 0;
    /// @}

    /// @name Reads (latest LWW across all peers)
    /// @{
    virtual std::optional<CapsRecord>
        caps_of(const std::string& machine_id) const = 0;
    virtual std::unordered_map<std::string, CapsRecord>
        all_caps() const = 0;
    virtual std::optional<LayoutRecord>
        current_layout() const = 0;
    virtual std::vector<WorkspaceRecord>
        all_workspaces() const = 0;
    virtual std::optional<ClipboardRecord>
        current_clipboard() const = 0;
    /// @}

    /// @brief Merge an incoming signed record received on the
    /// control connection. Rejects on signature failure, older
    /// version, or unknown schema.
    virtual bool merge_signed_bytes(Slot slot,
                                    const std::uint8_t* bytes,
                                    std::size_t len) = 0;

    /// @brief Serialize the local snapshot of @p slot for
    /// transmission to a peer.
    virtual std::vector<std::uint8_t>
        serialize_slot(Slot slot) const = 0;
};

}  // namespace xorio::orchestrator
