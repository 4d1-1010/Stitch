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

inline void put_u64(std::uint8_t* dst, std::uint64_t v) {
    put_u32(dst + 0, static_cast<std::uint32_t>( v        & 0xFFFFFFFFull));
    put_u32(dst + 4, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFull));
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

inline std::uint64_t read_u64(const std::uint8_t* src) {
    const std::uint64_t lo = read_u32(src + 0);
    const std::uint64_t hi = read_u32(src + 4);
    return lo | (hi << 32);
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
// Layout (all length-prefixed):
//   [src_len: u32][src: utf-8 bytes]
//   [text_len: u32][text: utf-8 bytes]
//   [html_len: u32][html: utf-8 bytes]               // empty if absent
//   [image_mime_len: u32][image_mime: utf-8 bytes]   // empty if no image
//   [image_bytes_len: u32][image_bytes: raw bytes]   // empty if no image
//
// Plain text is the fallback every receiver applies. HTML and
// image are optional — empty length means the source clipboard
// didn't carry that format (or the workspace's "Include rich
// text/images" toggle stripped them on the wire). The image
// MIME ("image/png", "image/jpeg", "image/bmp") rides the wire
// so the receiver can pick the right OS-clipboard format.

std::vector<std::uint8_t> encode_clipboard(const ClipboardUpdateMessage& m) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + m.source_machine.size()
                + 4 + m.text.size()
                + 4 + m.html.size()
                + 4 + m.image_mime.size()
                + 4 + m.image_bytes.size());
    std::uint8_t buf[4];

    put_u32(buf, static_cast<std::uint32_t>(m.source_machine.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.source_machine.data(), m.source_machine.size());

    put_u32(buf, static_cast<std::uint32_t>(m.text.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.text.data(), m.text.size());

    put_u32(buf, static_cast<std::uint32_t>(m.html.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.html.data(), m.html.size());

    put_u32(buf, static_cast<std::uint32_t>(m.image_mime.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.image_mime.data(), m.image_mime.size());

    put_u32(buf, static_cast<std::uint32_t>(m.image_bytes.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.image_bytes.data(), m.image_bytes.size());

    return out;
}

std::optional<ClipboardUpdateMessage>
decode_clipboard(const std::uint8_t* bytes, std::size_t len) {
    auto read_blob = [&](std::size_t off,
                          std::uint32_t& out_len)
                          -> std::optional<std::size_t> {
        if (len < off + 4) return std::nullopt;
        out_len = read_u32(bytes + off);
        if (out_len > kMaxPayload) return std::nullopt;
        if (len < off + 4 + out_len) return std::nullopt;
        return off + 4;
    };

    std::uint32_t src_len = 0, text_len = 0, html_len = 0;
    std::uint32_t mime_len = 0, img_len = 0;
    auto src_off  = read_blob(0, src_len);
    if (!src_off)  return std::nullopt;
    auto text_off = read_blob(*src_off  + src_len,  text_len);
    if (!text_off) return std::nullopt;
    auto html_off = read_blob(*text_off + text_len, html_len);
    if (!html_off) return std::nullopt;
    auto mime_off = read_blob(*html_off + html_len, mime_len);
    if (!mime_off) return std::nullopt;
    auto img_off  = read_blob(*mime_off + mime_len, img_len);
    if (!img_off)  return std::nullopt;

    ClipboardUpdateMessage m;
    m.source_machine.assign(
        reinterpret_cast<const char*>(bytes + *src_off), src_len);
    m.text.assign(
        reinterpret_cast<const char*>(bytes + *text_off), text_len);
    m.html.assign(
        reinterpret_cast<const char*>(bytes + *html_off), html_len);
    m.image_mime.assign(
        reinterpret_cast<const char*>(bytes + *mime_off), mime_len);
    m.image_bytes.assign(
        bytes + *img_off, bytes + *img_off + img_len);
    return m;
}

// ── Clipboard pull-model announce / fetch ─────────────────────
//
// ClipboardLatest payload:
//   [source_machine: lp-string]
//   [last_copy_t:    u64]
//   [flags:          u8]   ← bit 0 text, 1 html, 2 image, 3 files
//
// ClipboardFetch payload:
//   [requester:           lp-string]
//   [expected_last_copy_t: u64]
//
// Length-prefixed strings + put_u64 are defined further down in
// this file's anon namespace; we forward-declare locally so the
// codecs can sit next to their text-clipboard sibling.

namespace {
void put_lp_string(std::vector<std::uint8_t>&, const std::string&);
std::optional<std::string> read_lp_string(const std::uint8_t*,
                                            std::size_t,
                                            std::size_t&);
}  // namespace

std::vector<std::uint8_t>
encode_clipboard_latest(const ClipboardLatestMessage& m) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + m.source_machine.size() + 8 + 1);
    put_lp_string(out, m.source_machine);
    std::uint8_t buf[8];
    put_u64(buf, m.last_copy_t);
    append_bytes(out, buf, 8);
    std::uint8_t flags = 0;
    if (m.has_text)  flags |= 0x01;
    if (m.has_html)  flags |= 0x02;
    if (m.has_image) flags |= 0x04;
    if (m.has_files) flags |= 0x08;
    out.push_back(flags);
    return out;
}

std::optional<ClipboardLatestMessage>
decode_clipboard_latest(const std::uint8_t* bytes, std::size_t len) {
    std::size_t off = 0;
    auto src = read_lp_string(bytes, len, off);
    if (!src) return std::nullopt;
    if (len < off + 8 + 1) return std::nullopt;
    ClipboardLatestMessage m;
    m.source_machine = std::move(*src);
    m.last_copy_t    = read_u64(bytes + off); off += 8;
    const std::uint8_t flags = bytes[off];
    m.has_text  = (flags & 0x01) != 0;
    m.has_html  = (flags & 0x02) != 0;
    m.has_image = (flags & 0x04) != 0;
    m.has_files = (flags & 0x08) != 0;
    return m;
}

std::vector<std::uint8_t>
encode_clipboard_fetch(const ClipboardFetchMessage& m) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + m.requester_machine.size() + 8);
    put_lp_string(out, m.requester_machine);
    std::uint8_t buf[8];
    put_u64(buf, m.expected_last_copy_t);
    append_bytes(out, buf, 8);
    return out;
}

std::optional<ClipboardFetchMessage>
decode_clipboard_fetch(const std::uint8_t* bytes, std::size_t len) {
    std::size_t off = 0;
    auto req = read_lp_string(bytes, len, off);
    if (!req) return std::nullopt;
    if (len < off + 8) return std::nullopt;
    ClipboardFetchMessage m;
    m.requester_machine    = std::move(*req);
    m.expected_last_copy_t = read_u64(bytes + off);
    return m;
}

// ── File transfer ─────────────────────────────────────────────
//
// Common bookkeeping for the four file-transfer messages: a
// length-prefixed-string helper that handles the read-the-u32-
// then-validate-then-extract-bytes pattern in one place. Used
// by every decoder below to keep the bounds-check logic out of
// the per-message bodies.

namespace {

/// @brief Append a length-prefixed UTF-8 string to @p out.
void put_lp_string(std::vector<std::uint8_t>& out,
                    const std::string& s) {
    std::uint8_t buf[4];
    put_u32(buf, static_cast<std::uint32_t>(s.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, s.data(), s.size());
}

/// @brief Read a length-prefixed string starting at byte offset
/// @p off. On success, advances @p off past the string and
/// returns the bytes. On any failure (truncated buffer, length
/// exceeds @ref kMaxPayload) returns nullopt and @p off is
/// undefined.
std::optional<std::string>
read_lp_string(const std::uint8_t* bytes, std::size_t len,
                std::size_t& off) {
    if (len < off + 4) return std::nullopt;
    const std::uint32_t s_len = read_u32(bytes + off);
    if (s_len > kMaxPayload) return std::nullopt;
    if (len < off + 4 + s_len) return std::nullopt;
    std::string out;
    out.assign(reinterpret_cast<const char*>(bytes + off + 4), s_len);
    off += 4 + s_len;
    return out;
}

}  // namespace

// ── FileTransferStart ─────────────────────────────────────────
//
// Layout:
//   [transfer_id: u64]
//   [source_len: u32][source: utf-8 bytes]
//   [root_count: u32]
//   [for each root: [name_len: u32][name: utf-8 bytes]]
//   [file_count: u32]
//   [for each file: [path_len: u32][path: utf-8] [size: u64]]

std::vector<std::uint8_t>
encode_file_start(const FileTransferStartMessage& m) {
    std::vector<std::uint8_t> out;
    std::uint8_t buf[8];

    put_u64(buf, m.transfer_id);
    append_bytes(out, buf, 8);

    put_lp_string(out, m.source_machine);

    put_u32(buf, static_cast<std::uint32_t>(m.selection_roots.size()));
    append_bytes(out, buf, 4);
    for (const auto& r : m.selection_roots) put_lp_string(out, r);

    put_u32(buf, static_cast<std::uint32_t>(m.files.size()));
    append_bytes(out, buf, 4);
    for (const auto& f : m.files) {
        put_lp_string(out, f.relative_path);
        put_u64(buf, f.size);
        append_bytes(out, buf, 8);
    }
    return out;
}

std::optional<FileTransferStartMessage>
decode_file_start(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8) return std::nullopt;
    std::size_t off = 0;
    FileTransferStartMessage m;
    m.transfer_id = read_u64(bytes + off);
    off += 8;

    auto src = read_lp_string(bytes, len, off);
    if (!src) return std::nullopt;
    m.source_machine = std::move(*src);

    if (len < off + 4) return std::nullopt;
    const std::uint32_t root_count = read_u32(bytes + off);
    off += 4;
    if (root_count > kMaxPayload) return std::nullopt;
    m.selection_roots.reserve(root_count);
    for (std::uint32_t i = 0; i < root_count; ++i) {
        auto r = read_lp_string(bytes, len, off);
        if (!r) return std::nullopt;
        m.selection_roots.push_back(std::move(*r));
    }

    if (len < off + 4) return std::nullopt;
    const std::uint32_t file_count = read_u32(bytes + off);
    off += 4;
    if (file_count > kMaxPayload) return std::nullopt;
    m.files.reserve(file_count);
    for (std::uint32_t i = 0; i < file_count; ++i) {
        FileTransferEntry e;
        auto p = read_lp_string(bytes, len, off);
        if (!p) return std::nullopt;
        e.relative_path = std::move(*p);
        if (len < off + 8) return std::nullopt;
        e.size = read_u64(bytes + off);
        off += 8;
        m.files.push_back(std::move(e));
    }
    return m;
}

// ── FileChunk ─────────────────────────────────────────────────
//
// Layout:
//   [transfer_id: u64]
//   [file_index:  u32]
//   [is_last:     u8]
//   [data_len:    u32][data: bytes]

std::vector<std::uint8_t>
encode_file_chunk(const FileChunkMessage& m) {
    std::vector<std::uint8_t> out;
    out.reserve(8 + 4 + 1 + 4 + m.data.size());
    std::uint8_t buf[8];

    put_u64(buf, m.transfer_id);
    append_bytes(out, buf, 8);

    put_u32(buf, m.file_index);
    append_bytes(out, buf, 4);

    out.push_back(m.is_last ? 1 : 0);

    put_u32(buf, static_cast<std::uint32_t>(m.data.size()));
    append_bytes(out, buf, 4);
    append_bytes(out, m.data.data(), m.data.size());
    return out;
}

std::optional<FileChunkMessage>
decode_file_chunk(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8 + 4 + 1 + 4) return std::nullopt;
    std::size_t off = 0;
    FileChunkMessage m;
    m.transfer_id = read_u64(bytes + off);  off += 8;
    m.file_index  = read_u32(bytes + off);  off += 4;
    m.is_last     = (bytes[off] != 0);      off += 1;
    const std::uint32_t data_len = read_u32(bytes + off);
    off += 4;
    if (data_len > kMaxPayload) return std::nullopt;
    if (len < off + data_len) return std::nullopt;
    m.data.assign(bytes + off, bytes + off + data_len);
    return m;
}

// ── FileTransferEnd ───────────────────────────────────────────
//
// Layout: [transfer_id: u64].

std::vector<std::uint8_t>
encode_file_end(const FileTransferEndMessage& m) {
    std::vector<std::uint8_t> out(8);
    put_u64(out.data(), m.transfer_id);
    return out;
}

std::optional<FileTransferEndMessage>
decode_file_end(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8) return std::nullopt;
    FileTransferEndMessage m;
    m.transfer_id = read_u64(bytes);
    return m;
}

// ── FileTransferCancel ────────────────────────────────────────
//
// Layout: [transfer_id: u64][reason_len: u32][reason: utf-8].

std::vector<std::uint8_t>
encode_file_cancel(const FileTransferCancelMessage& m) {
    std::vector<std::uint8_t> out;
    out.reserve(8 + 4 + m.reason.size());
    std::uint8_t buf[8];
    put_u64(buf, m.transfer_id);
    append_bytes(out, buf, 8);
    put_lp_string(out, m.reason);
    return out;
}

std::optional<FileTransferCancelMessage>
decode_file_cancel(const std::uint8_t* bytes, std::size_t len) {
    if (len < 8) return std::nullopt;
    std::size_t off = 0;
    FileTransferCancelMessage m;
    m.transfer_id = read_u64(bytes + off);
    off += 8;
    auto r = read_lp_string(bytes, len, off);
    if (!r) return std::nullopt;
    m.reason = std::move(*r);
    return m;
}

}  // namespace unio_ui::orchestrator::control
