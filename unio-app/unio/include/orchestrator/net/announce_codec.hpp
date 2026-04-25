/// @file announce_codec.hpp
/// @brief Wire-format for LAN discovery announce datagrams.
///
/// Schema is intentionally kept identical to the Python tree's
/// `unio-peer-v1` JSON payload (see `unio/core/discovery.py`) so a
/// transitional mixed mesh — Python instances + ported C++
/// instances — discovers each other while the port is in flight.
///
/// Wire format, single JSON object per datagram:
///
///   {
///     "magic":      "unio-peer-v1",
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

namespace unio_ui::orchestrator::net {

/// @brief Magic string every valid announce datagram begins with.
inline constexpr const char* kAnnounceMagic = "unio-peer-v1";

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

/// @brief Decoded announce payload.
struct AnnouncePayload {
    std::string                   machine_id;   ///< Stable id of the announcer.
    std::string                   hostname;     ///< Display-friendly host name.
    std::uint16_t                 tcp_port = 0; ///< Mesh control-channel port.
    bool                          authed   = false; ///< Announcer reports a signed-in user.

    /// @brief Real geometry from the announcer's local probe.
    /// Empty when the wire payload omits the `displays_csv`
    /// extension (older Python instances + tests).
    std::vector<AnnounceDisplay>  displays;
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

}  // namespace unio_ui::orchestrator::net
