/// @file announce_codec.hpp
/// @brief Wire-format for LAN discovery announce datagrams.
///
/// Schema is intentionally kept identical to the Python tree's
/// `xorio-peer-v1` JSON payload (see `xorio/core/discovery.py`) so a
/// transitional mixed mesh — Python instances + ported C++
/// instances — discovers each other while the port is in flight.
///
/// Wire format, single JSON object per datagram:
///
///   {
///     "magic":      "xorio-peer-v1",
///     "machine_id": "<string>",
///     "hostname":   "<string>",
///     "tcp_port":   <int>,
///     "authed":     <bool>
///   }
///
/// Scope: pure functions over byte buffers — no I/O, no logging.
/// Hand-rolled (no external JSON dep) because the schema is
/// fixed-shape and ~5 fields wide; see `feedback_minimal_deps`.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace xorio::orchestrator::net {

/// @brief Magic string every valid announce datagram begins with.
inline constexpr const char* kAnnounceMagic = "xorio-peer-v1";

/// @brief UDP port the wire format reserves for announces. Matches
/// the Python tree's `DISCOVERY_PORT`.
inline constexpr std::uint16_t kAnnouncePort = 24801;

/// @brief One display reported by a peer's announce.
///
/// Geometry is in the announcing peer's own desktop coordinates;
/// the orchestrator aggregates these into the mesh CRDT verbatim
/// (each peer's displays live in their own coordinate space).
struct AnnounceDisplay {
    std::string   monitor_id;     ///< OS display name (eDP-1, \\\\.\\DISPLAY1, …).
    std::int32_t  global_x = 0;
    std::int32_t  global_y = 0;
    std::int32_t  width    = 0;
    std::int32_t  height   = 0;
};

/// @brief One workspace record on the wire. The catalogue is the
/// announcing peer's full local view, including tombstones; the
/// listener applies last-writer-wins keyed on @ref version_ns.
struct AnnounceWorkspace {
    std::string                id;
    std::string                name;
    std::uint64_t              version_ns = 0;
    bool                       tombstone  = false;
    std::vector<std::string>   members;            ///< union (legacy compat).
    std::vector<std::string>   input_members;
    std::vector<std::string>   clipboard_members;

    // ── Settings ──────────────────────────────────────────────
    // Mirrors the in-memory Workspace struct. Encoded as small
    // unsigned ints / bools after the member list; see
    // encode_workspaces_v1 in announce_codec.cpp.
    std::uint8_t   clipboard_max          = 1;   // 0..4 ClipboardLimit
    bool           clipboard_rich         = true;
    bool           clipboard_files        = false;
    std::int32_t   cursor_edge_margin     = 4;
    bool           cursor_require_modifier = false;
    bool           cursor_block_hotkeys    = false;
    std::uint8_t   auto_unlock            = 0;   // 0..3 AutoUnlock

    /// @brief Per-monitor mesh-global positions arranged by the
    /// user on the Layout tab. Each entry travels as
    /// `<machine_id_len>\n<machine_id><monitor_id_len>\n<monitor_id>
    /// <global_x>\n<global_y>\n` after the settings block.
    struct LayoutEntry {
        std::string  machine_id;
        std::string  monitor_id;
        std::int32_t global_x = 0;
        std::int32_t global_y = 0;
    };
    std::vector<LayoutEntry> layout;
};

/// @brief Decoded announce payload.
struct AnnouncePayload {
    std::string                   machine_id;   ///< Stable id of the announcer.
    std::string                   hostname;     ///< Display-friendly host name.
    std::uint16_t                 tcp_port  = 0; ///< Mesh control-channel port.
    /// @brief Data-channel port (file-transfer chunks). Optional
    /// for forward-compat — older announcers omit it; receivers
    /// that don't see this key fall back to the control port and
    /// pay the cursor-latency cost during transfers.
    std::uint16_t                 data_port = 0;
    bool                          authed   = false; ///< Announcer reports a signed-in user.

    /// @brief Real geometry from the announcer's local probe.
    /// Empty when the wire payload omits the `displays_csv`
    /// extension (older Python instances + tests).
    std::vector<AnnounceDisplay>  displays;

    /// @brief Monotonically-increasing counter the announcer
    /// bumps every time its user clicks Identify. Receivers fire
    /// their own local Identify overlays when they observe a
    /// higher value than the last one they saw from this peer.
    /// 0 = never requested.
    std::uint64_t                 identify_request_id = 0;

    /// @brief Full workspace catalogue (including tombstones) as
    /// of the announce. Empty when the wire payload omits the
    /// `workspaces_v1` extension. Receivers feed this to
    /// @ref IWorkspaceManager::merge_remote.
    std::vector<AnnounceWorkspace> workspaces;
};

/// @brief Serialise @p p to a JSON datagram body. Output is
/// deterministic — same input produces byte-identical output, key
/// order matches @ref AnnouncePayload's declaration order.
std::vector<std::uint8_t> encode_announce(const AnnouncePayload& p);

/// @brief Parse @p bytes back into an @ref AnnouncePayload.
/// @return Filled payload on success; `std::nullopt` if the bytes
/// are not valid UTF-8 JSON, the magic mismatches, or any required
/// field is missing / mistyped.
std::optional<AnnouncePayload>
decode_announce(const std::uint8_t* bytes, std::size_t len);

}  // namespace xorio::orchestrator::net
