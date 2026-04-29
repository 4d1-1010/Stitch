/// @file protocol.hpp
/// @brief Wire format for the peer-to-peer control channel.
///
/// Scope: pure data definitions + binary encode / decode helpers.
/// No I/O, no sockets — the TCP transport in
/// `tcp_control_channel.cpp` is built on top of this. Frame layout
/// is intentionally tiny + self-contained so a future Wayland or
/// Windows transport can reuse the same codec verbatim.
///
/// Frame on the wire (little-endian):
///
///     [type:    u16]
///     [length:  u32]   ← payload byte count, capped at kMaxPayload
///     [payload: bytes] ← message-specific binary blob
///
/// Each message type has its own fixed binary payload — no JSON, no
/// schema versioning beyond the @ref kProtocolVersion handshake
/// inside @ref HelloMessage. Both peers run the same build during
/// Phase A so we don't pay for forward-compat machinery yet.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace unio_ui::orchestrator::control {

/// @brief All control-channel message types the wire understands.
enum class MessageType : std::uint16_t {
    Hello        = 0x0001,
    Heartbeat    = 0x0002,
    MouseMoveAbs = 0x0010,
    MouseButton  = 0x0011,
    MouseScroll  = 0x0012,
    MouseRel     = 0x0013,
    KeyEvent     = 0x0014,
    Handoff      = 0x0020,
};

/// @brief Header byte count for a frame.
inline constexpr std::size_t kFrameHeaderSize = 6;

/// @brief Cap on payload size — covers every Phase A message
/// comfortably; oversized frames are rejected as malformed.
inline constexpr std::size_t kMaxPayload = 64 * 1024;

/// @brief Protocol version sent in the Hello handshake.
inline constexpr std::uint16_t kProtocolVersion = 1;

// ── Message structs ───────────────────────────────────────────

struct HelloMessage {
    std::string   machine_id;
    std::uint16_t protocol_version = kProtocolVersion;
};

struct HeartbeatMessage {
    std::uint64_t seq = 0;
};

/// @brief Absolute mouse position in the mesh-wide global
/// coordinate space (the same coords the Layout tab arranges).
struct MouseMoveAbsMessage {
    std::int32_t global_x = 0;
    std::int32_t global_y = 0;
};

struct MouseButtonMessage {
    std::uint8_t button  = 0;   ///< 1=left, 2=middle, 3=right.
    bool         pressed = false;
};

struct MouseScrollMessage {
    std::int32_t dx = 0;
    std::int32_t dy = 0;       ///< +y = scroll up.
};

/// @brief Relative mouse motion forwarded by a *dormant* peer.
/// The dormant peer pins its local cursor at the handoff edge
/// and reports the user's continuing mouse deltas so the active
/// peer can apply them to its own cursor.
struct MouseRelMessage {
    std::int32_t dx = 0;
    std::int32_t dy = 0;
};

/// @brief Keyboard event forwarded by the dormant peer. The
/// scancode field is a USB HID Usage ID (Keyboard/Keypad page
/// 0x07). Each platform's raw-capture translates its native
/// code (evdev on Linux, distinguished VK on Windows) into HID
/// at the boundary, and inject_key reverses the lookup —
/// see @ref keycodes.hpp.
struct KeyEventMessage {
    std::uint32_t scancode = 0;
    bool          pressed  = false;
};

/// @brief "Take the cursor" message. Sent by the currently-active
/// peer when its cursor crosses a monitor edge that maps to one
/// of @p target_peer's monitors. The receiver becomes the active
/// peer and warps its local cursor to (@ref entry_x, @ref entry_y)
/// — coords already translated into the receiver's local OS
/// virtual screen space by the sender.
struct HandoffMessage {
    std::int32_t entry_x = 0;
    std::int32_t entry_y = 0;
};

// ── Frame encode / decode ─────────────────────────────────────

/// @brief Wrap @p payload in a frame header. Caller writes the
/// returned bytes to the socket as a single send().
std::vector<std::uint8_t> encode_frame(MessageType type,
                                        const std::uint8_t* payload,
                                        std::size_t payload_len);

/// @brief Read the 6-byte frame header. Returns nullopt if the
/// length field exceeds @ref kMaxPayload.
struct FrameHeader {
    MessageType   type;
    std::uint32_t payload_len;
};
std::optional<FrameHeader> decode_frame_header(const std::uint8_t* bytes,
                                                std::size_t len);

// ── Per-message payload codecs ────────────────────────────────
//
// Each pair (encode_x → bytes, decode_x → optional<x>) is
// symmetric. Encoders never fail; decoders return nullopt if the
// payload bytes are too short or otherwise malformed.

std::vector<std::uint8_t> encode_hello       (const HelloMessage&);
std::vector<std::uint8_t> encode_heartbeat   (const HeartbeatMessage&);
std::vector<std::uint8_t> encode_mouse_move  (const MouseMoveAbsMessage&);
std::vector<std::uint8_t> encode_mouse_button(const MouseButtonMessage&);
std::vector<std::uint8_t> encode_mouse_scroll(const MouseScrollMessage&);
std::vector<std::uint8_t> encode_mouse_rel   (const MouseRelMessage&);
std::vector<std::uint8_t> encode_key_event   (const KeyEventMessage&);
std::vector<std::uint8_t> encode_handoff     (const HandoffMessage&);

std::optional<HelloMessage>        decode_hello       (const std::uint8_t*, std::size_t);
std::optional<HeartbeatMessage>    decode_heartbeat   (const std::uint8_t*, std::size_t);
std::optional<MouseMoveAbsMessage> decode_mouse_move  (const std::uint8_t*, std::size_t);
std::optional<MouseButtonMessage>  decode_mouse_button(const std::uint8_t*, std::size_t);
std::optional<MouseScrollMessage>  decode_mouse_scroll(const std::uint8_t*, std::size_t);
std::optional<MouseRelMessage>     decode_mouse_rel   (const std::uint8_t*, std::size_t);
std::optional<KeyEventMessage>     decode_key_event   (const std::uint8_t*, std::size_t);
std::optional<HandoffMessage>      decode_handoff     (const std::uint8_t*, std::size_t);

}  // namespace unio_ui::orchestrator::control
