/// @file protocol.cpp
/// @brief Binary encode / decode for the control-channel wire
/// format declared in @ref protocol.hpp. Pure functions over byte
/// buffers — no I/O, no allocations beyond the returned vector.

#include "orchestrator/control/protocol.hpp"

#include <cstring>

namespace unio_ui::orchestrator::control {

namespace {

// ── Little-endian primitive read / write ──────────────────────

inline void put_u16(std::uint8_t* dst, std::uint16_t v) {
    dst[0] = static_cast<std::uint8_t>( v       & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

inline void put_u32(std::uint8_t* dst, std::uint32_t v) {
    dst[0] = static_cast<std::uint8_t>( v        & 0xFFu);
    dst[1] = static_cast<std::uint8_t>((v >>  8) & 0xFFu);
    dst[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

inline void put_i32(std::uint8_t* dst, std::int32_t v) {
    put_u32(dst, static_cast<std::uint32_t>(v));
}

inline std::uint16_t read_u16(const std::uint8_t* src) {
    return static_cast<std::uint16_t>(src[0])
         | (static_cast<std::uint16_t>(src[1]) << 8);
}

inline std::uint32_t read_u32(const std::uint8_t* src) {
    return  static_cast<std::uint32_t>(src[0])
         | (static_cast<std::uint32_t>(src[1]) << 8)
         | (static_cast<std::uint32_t>(src[2]) << 16)
         | (static_cast<std::uint32_t>(src[3]) << 24);
}

inline std::int32_t read_i32(const std::uint8_t* src) {
    return static_cast<std::int32_t>(read_u32(src));
}

inline void append_bytes(std::vector<std::uint8_t>& out,
                         const void* data, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

}  // namespace

// ── Frame header ──────────────────────────────────────────────

std::vector<std::uint8_t> encode_frame(MessageType type,
                                        const std::uint8_t* payload,
                                        std::size_t payload_len) {
    std::vector<std::uint8_t> out;
    out.resize(kFrameHeaderSize + payload_len);
    put_u16(out.data() + 0, static_cast<std::uint16_t>(type));
    put_u32(out.data() + 2, static_cast<std::uint32_t>(payload_len));
    if (payload_len > 0 && payload != nullptr) {
        std::memcpy(out.data() + kFrameHeaderSize, payload, payload_len);
    }
    return out;
}

std::optional<FrameHeader>
decode_frame_header(const std::uint8_t* bytes, std::size_t len) {
    if (len < kFrameHeaderSize) return std::nullopt;
    FrameHeader h;
    h.type        = static_cast<MessageType>(read_u16(bytes + 0));
    h.payload_len = read_u32(bytes + 2);
    if (h.payload_len > kMaxPayload) return std::nullopt;
    return h;
}

// ── Hello ─────────────────────────────────────────────────────
//
// Layout: [machine_id_len: u32][machine_id: bytes][version: u16].

std::vector<std::uint8_t> encode_hello(const HelloMessage& m) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + m.machine_id.size() + 2);
    std::uint8_t buf[4];
    put_u32(buf, static_cast<std::uint32_t>(m.machine_id.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.machine_id.data(), m.machine_id.size());
    put_u16(buf, m.protocol_version);
    append_bytes(out, buf, 2);
    return out;
}

std::optional<HelloMessage>
decode_hello(const std::uint8_t* bytes, std::size_t len) {
    if (len < 4) return std::nullopt;
    const std::uint32_t mid_len = read_u32(bytes + 0);
    if (mid_len > kMaxPayload) return std::nullopt;
    if (len < 4 + mid_len + 2) return std::nullopt;
    HelloMessage m;
    m.machine_id.assign(reinterpret_cast<const char*>(bytes + 4), mid_len);
    m.protocol_version = read_u16(bytes + 4 + mid_len);
    return m;
}

// ── Heartbeat ─────────────────────────────────────────────────
//
// Layout: [seq: u64].

std::vector<std::uint8_t> encode_heartbeat(const HeartbeatMessage& m) {
    std::vector<std::uint8_t> out(8);
    put_u32(out.data() + 0, static_cast<std::uint32_t>( m.seq        & 0xFFFFFFFFu));
    put_u32(out.data() + 4, static_cast<std::uint32_t>((m.seq >> 32) & 0xFFFFFFFFu));
    return out;
}

std::optional<HeartbeatMessage>
decode_heartbeat(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8) return std::nullopt;
    HeartbeatMessage m;
    const std::uint64_t lo = read_u32(bytes + 0);
    const std::uint64_t hi = read_u32(bytes + 4);
    m.seq = lo | (hi << 32);
    return m;
}

// ── Mouse — fixed binary layouts ──────────────────────────────

std::vector<std::uint8_t> encode_mouse_move(const MouseMoveAbsMessage& m) {
    std::vector<std::uint8_t> out(8);
    put_i32(out.data() + 0, m.global_x);
    put_i32(out.data() + 4, m.global_y);
    return out;
}

std::optional<MouseMoveAbsMessage>
decode_mouse_move(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8) return std::nullopt;
    MouseMoveAbsMessage m;
    m.global_x = read_i32(bytes + 0);
    m.global_y = read_i32(bytes + 4);
    return m;
}

std::vector<std::uint8_t> encode_mouse_button(const MouseButtonMessage& m) {
    std::vector<std::uint8_t> out(2);
    out[0] = m.button;
    out[1] = m.pressed ? 1 : 0;
    return out;
}

std::optional<MouseButtonMessage>
decode_mouse_button(const std::uint8_t* bytes, std::size_t len) {
    if (len < 2) return std::nullopt;
    MouseButtonMessage m;
    m.button  = bytes[0];
    m.pressed = (bytes[1] != 0);
    return m;
}

std::vector<std::uint8_t> encode_mouse_scroll(const MouseScrollMessage& m) {
    std::vector<std::uint8_t> out(8);
    put_i32(out.data() + 0, m.dx);
    put_i32(out.data() + 4, m.dy);
    return out;
}

std::optional<MouseScrollMessage>
decode_mouse_scroll(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8) return std::nullopt;
    MouseScrollMessage m;
    m.dx = read_i32(bytes + 0);
    m.dy = read_i32(bytes + 4);
    return m;
}

// ── MouseRel ──────────────────────────────────────────────────
//
// Layout: [dx: i32][dy: i32]. Same on-the-wire shape as scroll;
// kept as a separate codec so the message types stay
// self-describing and we can evolve them independently.

std::vector<std::uint8_t> encode_mouse_rel(const MouseRelMessage& m) {
    std::vector<std::uint8_t> out(8);
    put_i32(out.data() + 0, m.dx);
    put_i32(out.data() + 4, m.dy);
    return out;
}

std::optional<MouseRelMessage>
decode_mouse_rel(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8) return std::nullopt;
    MouseRelMessage m;
    m.dx = read_i32(bytes + 0);
    m.dy = read_i32(bytes + 4);
    return m;
}

// ── KeyEvent ──────────────────────────────────────────────────
//
// Layout: [scancode: u32][pressed: u8]. Total 5 bytes.

std::vector<std::uint8_t> encode_key_event(const KeyEventMessage& m) {
    std::vector<std::uint8_t> out(5);
    put_i32(out.data() + 0, static_cast<std::int32_t>(m.scancode));
    out[4] = m.pressed ? 1 : 0;
    return out;
}

std::optional<KeyEventMessage>
decode_key_event(const std::uint8_t* bytes, std::size_t len) {
    if (len < 5) return std::nullopt;
    KeyEventMessage m;
    m.scancode = static_cast<std::uint32_t>(read_i32(bytes + 0));
    m.pressed  = (bytes[4] != 0);
    return m;
}

// ── Handoff ───────────────────────────────────────────────────
//
// Layout: [entry_x: i32][entry_y: i32].

std::vector<std::uint8_t> encode_handoff(const HandoffMessage& m) {
    std::vector<std::uint8_t> out(8);
    put_i32(out.data() + 0, m.entry_x);
    put_i32(out.data() + 4, m.entry_y);
    return out;
}

std::optional<HandoffMessage>
decode_handoff(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8) return std::nullopt;
    HandoffMessage m;
    m.entry_x = read_i32(bytes + 0);
    m.entry_y = read_i32(bytes + 4);
    return m;
}

// ── ClipboardUpdate ───────────────────────────────────────────
//
// Layout: [src_len: u32][src: bytes][content_len: u32][content: bytes].
// content is UTF-8 plain text (no rich-text or file payload yet).

std::vector<std::uint8_t> encode_clipboard(const ClipboardUpdateMessage& m) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + m.source_machine.size() + 4 + m.content.size());
    std::uint8_t buf[4];
    put_u32(buf, static_cast<std::uint32_t>(m.source_machine.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.source_machine.data(), m.source_machine.size());
    put_u32(buf, static_cast<std::uint32_t>(m.content.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.content.data(), m.content.size());
    return out;
}

std::optional<ClipboardUpdateMessage>
decode_clipboard(const std::uint8_t* bytes, std::size_t len) {
    if (len < 4) return std::nullopt;
    const std::uint32_t src_len = read_u32(bytes + 0);
    if (src_len > kMaxPayload) return std::nullopt;
    if (len < 4 + src_len + 4) return std::nullopt;
    const std::uint32_t content_len = read_u32(bytes + 4 + src_len);
    if (content_len > kMaxPayload) return std::nullopt;
    if (len < 4 + src_len + 4 + content_len) return std::nullopt;
    ClipboardUpdateMessage m;
    m.source_machine.assign(
        reinterpret_cast<const char*>(bytes + 4), src_len);
    m.content.assign(
        reinterpret_cast<const char*>(bytes + 4 + src_len + 4),
        content_len);
    return m;
}

}  // namespace unio_ui::orchestrator::control
