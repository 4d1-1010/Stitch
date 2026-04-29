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
    Hello                = 0x0001,
    Heartbeat            = 0x0002,
    MouseMoveAbs         = 0x0010,
    MouseButton          = 0x0011,
    MouseScroll          = 0x0012,
    MouseRel             = 0x0013,
    KeyEvent             = 0x0014,
    Handoff              = 0x0020,
    ClipboardUpdate      = 0x0030,
    /// @brief Source's "I just did a Ctrl+C" announcement. Tiny —
    /// no payload bytes, just metadata. Receivers track who has
    /// the freshest content; the actual bytes flow only on demand.
    ClipboardLatest      = 0x0031,
    /// @brief Receiver's "send me your clipboard" pull request,
    /// triggered when the cursor arrives on this peer. Source
    /// replies with @ref ClipboardUpdateMessage (text/html/
    /// image) and / or a @ref FileTransferStart sequence
    /// addressed only to the requester.
    ClipboardFetch       = 0x0032,
    FileTransferStart    = 0x0040,
    FileChunk            = 0x0041,
    FileTransferEnd      = 0x0042,
    FileTransferCancel   = 0x0043,
};

/// @brief Header byte count for a frame.
inline constexpr std::size_t kFrameHeaderSize = 6;

/// @brief Cap on payload size — covers every message comfortably,
/// including the workspace-configurable max-clipboard-text size
/// (10 MB ceiling on the UI, plus header overhead). Oversized
/// frames are rejected as malformed.
inline constexpr std::size_t kMaxPayload = 16 * 1024 * 1024;

/// @brief Default @ref FileChunkMessage::data size produced by
/// the source. Tuned for low send-mutex hold time so cursor /
/// keystroke messages naturally interleave between chunks
/// without perceptible lag — ~5 ms on gigabit, ~50 ms on
/// 100 Mbit. The receiver doesn't enforce this exact value;
/// any size up to @ref kMaxPayload is accepted.
inline constexpr std::size_t kFileChunkSize = 64 * 1024;

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

/// @brief Shared-clipboard update broadcast by a peer whose
/// local clipboard just changed. Carries plain text plus
/// optional rich-text (HTML) and image (PNG) payloads — the
/// receiver applies whichever its destination app accepts.
/// The workspace's "Include rich text/images" toggle gates
/// whether @c html and @c image_png are non-empty on the
/// wire; when the toggle is off the source strips them and
/// the receiver only sees @c text. @c source_machine names
/// the originator so the receiver doesn't bounce its own
/// outbound back.
struct ClipboardUpdateMessage {
    std::string               source_machine;
    std::string               text;
    std::string               html;
    /// @brief Image MIME — "image/png", "image/jpeg",
    /// "image/bmp" — empty when no image is carried.
    std::string               image_mime;
    /// @brief Raw image bytes in the format named by
    /// @ref image_mime. Empty when no image is carried.
    std::vector<std::uint8_t> image_bytes;
};

/// @brief Tiny "I just copied something" notification. Broadcast
/// to every peer in the workspace whose source is allowed to
/// send clipboard updates. Carries only the source's identity
/// + monotonic copy counter so receivers can decide whether
/// the source is fresher than what they already have. The
/// actual payload (text/html/image/files) is fetched lazily
/// when the cursor arrives on a peer that wants to paste.
struct ClipboardLatestMessage {
    std::string   source_machine;
    /// @brief Monotonic, per-peer counter that advances on
    /// every local Ctrl+C the orchestrator captures. Receivers
    /// compare this against their own latest known value for
    /// the same source to decide whether to refetch.
    std::uint64_t last_copy_t = 0;
    /// @brief Hint flags so receivers can size up the work
    /// before requesting bytes. Not authoritative — the source
    /// re-reads its current clipboard at fetch time, so a peer
    /// that copied text + an image gets both regardless of what
    /// these flags said at announce time.
    bool          has_text    = false;
    bool          has_html    = false;
    bool          has_image   = false;
    bool          has_files   = false;
};

/// @brief Receiver-initiated pull. Sent from the peer where the
/// user is about to paste (cursor lives there) to the peer that
/// last broadcast a @ref ClipboardLatestMessage. The source
/// reads its current local clipboard and replies with
/// @ref ClipboardUpdateMessage (for text/html/image) and / or
/// a @ref FileTransferStartMessage sequence (for files),
/// addressed only to @ref requester_machine.
struct ClipboardFetchMessage {
    /// @brief machine_id of the peer asking for content. The
    /// source uses this to address the reply (instead of
    /// broadcasting it to the whole workspace).
    std::string   requester_machine;
    /// @brief @ref ClipboardLatestMessage::last_copy_t the
    /// requester saw. Source ignores the request if its
    /// current clipboard is older — the receiver must have
    /// missed a newer announcement and will catch up on the
    /// next Latest broadcast.
    std::uint64_t expected_last_copy_t = 0;
};

// ── File transfer ─────────────────────────────────────────────
//
// File copy/paste between PCs is delivered as a streamed sequence
// of frames on the same TCP control channel as cursor / keys /
// clipboard text. Source generates a random @c transfer_id, sends
// a FileTransferStart with metadata for every file in the user's
// selection, then a sequence of FileChunk frames (default 64 KB
// data each) that the receiver appends to the corresponding
// pre-allocated file in a temp dir. A trailing FileTransferEnd
// signals all chunks are delivered — the receiver only sets the
// OS clipboard with the file URIs after that, so a paste
// mid-transfer never finds half-written files.
//
// The 64 KB chunk size is a latency / throughput trade-off: the
// control channel's send mutex holds for ~5 ms per chunk on
// gigabit, so cursor / keystroke messages naturally interleave
// between chunks without perceptible lag. Bigger chunks would
// reduce framing overhead at the cost of input pauses while a
// chunk is mid-flight.

/// @brief One file in a transfer's manifest. Sizes are advertised
/// up-front so the receiver can pre-create files at their final
/// length and detect when the transfer for each file is complete.
struct FileTransferEntry {
    /// @brief Path the receiver materialises @em under its
    /// per-transfer temp dir. Forward-slash separated; includes
    /// any subfolder structure from the source's selection
    /// (e.g. @c "myfolder/sub/foo.txt"). Backslashes are
    /// normalised to forward slashes on the source side so the
    /// receiver sees the same shape regardless of OS.
    std::string   relative_path;
    /// @brief File size in bytes. Receiver sums chunk lengths
    /// to confirm the file is fully delivered.
    std::uint64_t size = 0;
};

/// @brief Announces a file transfer. Carries every file's
/// metadata in one frame so the receiver can lay out the temp
/// directory before the first chunk lands.
struct FileTransferStartMessage {
    /// @brief Random u64 — receiver uses this to disambiguate
    /// concurrent transfers (a user could copy a second
    /// selection while the first is still streaming).
    std::uint64_t                  transfer_id = 0;
    /// @brief Origin peer's machine_id. Used for the on-screen
    /// progress overlay's "from / to" label.
    std::string                    source_machine;
    /// @brief Top-level names the user actually selected (file
    /// or folder basenames). The receiver puts these on its
    /// OS clipboard after the transfer ends; the file manager
    /// uses them as the names of the pasted entries.
    std::vector<std::string>       selection_roots;
    /// @brief Flat list of every leaf file that needs to be
    /// shipped — folders are flattened to their contents.
    std::vector<FileTransferEntry> files;
};

/// @brief One slice of a file's bytes. Frames for one transfer
/// arrive in file_index / offset order — the receiver appends
/// to the matching open file handle without seeking.
struct FileChunkMessage {
    std::uint64_t              transfer_id = 0;
    /// @brief Index into @ref FileTransferStartMessage::files
    /// the chunk belongs to.
    std::uint32_t              file_index  = 0;
    /// @brief @c true on the last chunk of this file. Receiver
    /// closes the file handle once it sees this.
    bool                       is_last     = false;
    /// @brief Raw file bytes. Length never exceeds 64 KB by
    /// convention; the receiver caps at @ref kMaxPayload as
    /// a defence against a misbehaving source.
    std::vector<std::uint8_t>  data;
};

/// @brief Source's signal that every file's chunks have been
/// delivered. Receiver finalises by setting the OS clipboard
/// to point at the materialised temp paths.
struct FileTransferEndMessage {
    std::uint64_t transfer_id = 0;
};

/// @brief Source aborts a transfer (user clicked Cancel on the
/// progress overlay, source process is shutting down, etc.).
/// Receiver tears down its open file handles and recursively
/// removes the per-transfer temp dir.
struct FileTransferCancelMessage {
    std::uint64_t transfer_id = 0;
    /// @brief Free-form description for the receiver's log
    /// (and a future status surface). Empty is allowed.
    std::string   reason;
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
std::vector<std::uint8_t> encode_handoff           (const HandoffMessage&);
std::vector<std::uint8_t> encode_clipboard         (const ClipboardUpdateMessage&);
std::vector<std::uint8_t> encode_clipboard_latest  (const ClipboardLatestMessage&);
std::vector<std::uint8_t> encode_clipboard_fetch   (const ClipboardFetchMessage&);
std::vector<std::uint8_t> encode_file_start        (const FileTransferStartMessage&);
std::vector<std::uint8_t> encode_file_chunk        (const FileChunkMessage&);
std::vector<std::uint8_t> encode_file_end          (const FileTransferEndMessage&);
std::vector<std::uint8_t> encode_file_cancel       (const FileTransferCancelMessage&);

std::optional<HelloMessage>                decode_hello        (const std::uint8_t*, std::size_t);
std::optional<HeartbeatMessage>            decode_heartbeat    (const std::uint8_t*, std::size_t);
std::optional<MouseMoveAbsMessage>         decode_mouse_move   (const std::uint8_t*, std::size_t);
std::optional<MouseButtonMessage>          decode_mouse_button (const std::uint8_t*, std::size_t);
std::optional<MouseScrollMessage>          decode_mouse_scroll (const std::uint8_t*, std::size_t);
std::optional<MouseRelMessage>             decode_mouse_rel    (const std::uint8_t*, std::size_t);
std::optional<KeyEventMessage>             decode_key_event    (const std::uint8_t*, std::size_t);
std::optional<HandoffMessage>              decode_handoff      (const std::uint8_t*, std::size_t);
std::optional<ClipboardUpdateMessage>      decode_clipboard    (const std::uint8_t*, std::size_t);
std::optional<ClipboardLatestMessage>      decode_clipboard_latest(const std::uint8_t*, std::size_t);
std::optional<ClipboardFetchMessage>       decode_clipboard_fetch (const std::uint8_t*, std::size_t);
std::optional<FileTransferStartMessage>    decode_file_start   (const std::uint8_t*, std::size_t);
std::optional<FileChunkMessage>            decode_file_chunk   (const std::uint8_t*, std::size_t);
std::optional<FileTransferEndMessage>      decode_file_end     (const std::uint8_t*, std::size_t);
std::optional<FileTransferCancelMessage>   decode_file_cancel  (const std::uint8_t*, std::size_t);

}  // namespace unio_ui::orchestrator::control
