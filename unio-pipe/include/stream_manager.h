#pragma once

#include "decoder.h"
#include "encoder.h"
#include "frame.h"
#include "presenter.h"
#include "quic_transport.h"
#include "spsc_ring.h"
#include "unio_pipe.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace unio {

// One active outbound stream. Owns the capture thread + the
// SPSC ring between capture and (future) encoder. No encoder
// yet — Day 2 drains the ring and counts frames so we can see
// the capture path actually running.
//
// Day 3 replaces the drain thread with the VA-API encoder;
// Day 4 plugs msquic on the other side. Each slot is behind a
// strict interface so swapping it doesn't ripple.
struct OutboundStream {
    std::string stream_id;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> dropped_at_ring{0};
    std::atomic<std::uint64_t> encoded{0};
    std::atomic<std::uint64_t> dropped_at_send{0};
    std::atomic<std::uint64_t> bytes_emitted{0};
    SpscRing<CpuFrame, 2> frame_ring;
    SpscRing<EncodedPacket, 4> packet_ring;
    std::thread encode_thread;
    std::thread send_thread;

    std::unique_ptr<Encoder> encoder;
    std::unique_ptr<QuicOutbound> quic;

    // Opt-in Annex-B dump for bring-up validation. Populated from
    // $UNIO_PIPE_BITSTREAM_DUMP at StartOutbound — when set, every
    // packet's NAL bytes are appended to the file before the send
    // thread drops them. Lets a developer point ffmpeg at the dump
    // and confirm the encoder is producing decodable H.264 without
    // needing msquic / a real sink wired up yet.
    std::FILE* dump_file = nullptr;

    // Per-OS capture — opaque pointer because the Windows side
    // will want IDirect3DDevice references we'd rather not pull
    // into this header.
    struct Capture;
    std::unique_ptr<Capture> capture;

    OutboundStream();
    ~OutboundStream();
};

class StreamManager {
public:
    StreamManager();
    ~StreamManager();

    // Called by control_socket on a start_outbound command.
    // peer_host / peer_port address the QUIC sink; on empty host
    // the outbound stays in loopback-dump mode (no network I/O).
    // Returns nullopt on success, otherwise an error string.
    std::optional<std::string> StartOutbound(
        std::string_view stream_id,
        std::string_view monitor_source,
        std::string_view peer_host,
        int peer_port,
        int width, int height, int fps,
        int capture_x = 0, int capture_y = 0);

    // Listen on `port` and write received H.264 packets to a
    // dump file (path picked by $UNIO_PIPE_BITSTREAM_DUMP so
    // reuse the same validation plumbing). window_w/h are the
    // presenter rect — zero means "primary monitor full size".
    // Small values (e.g. 640x360) are useful for loopback testing
    // where the sink's override-redirect window would otherwise
    // occlude whatever the colocated source is capturing.
    // Lifetime bound to the StreamManager; Stop() cancels both
    // directions.
    std::optional<std::string> StartInbound(
        std::string_view stream_id, int port,
        int window_w, int window_h);

    std::optional<std::string> Stop(std::string_view stream_id);

    // helper_status payload: one entry per live stream.
    struct StatusEntry {
        std::string stream_id;
        std::string direction;     // "outbound" or "inbound"
        std::uint64_t frames_captured = 0;
        std::uint64_t frames_dropped_at_ring = 0;
        std::uint64_t frames_encoded = 0;
        std::uint64_t packets_dropped_at_send = 0;
        std::uint64_t bytes_emitted = 0;
        std::uint64_t packets_received = 0;
        std::uint64_t bytes_received = 0;
        std::uint64_t frames_decoded = 0;
        std::uint64_t frames_presented = 0;
        std::uint32_t decode_width = 0;
        std::uint32_t decode_height = 0;
        std::string encoder;
        std::string decoder;
        std::string presenter;
        std::string peer;
        bool quic_connected = false;
    };
    std::vector<StatusEntry> Status() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<OutboundStream>>
        outbound_;

    // Inbound streams use a separate map; their lifecycle is
    // decoupled from capture/encode.
    struct InboundStream {
        std::string stream_id;
        std::unique_ptr<QuicInbound> quic;
        std::unique_ptr<Decoder> decoder;
        std::unique_ptr<Presenter> presenter;
        std::FILE* dump_file = nullptr;
        std::atomic<std::uint64_t> bytes_written{0};
        std::atomic<std::uint64_t> frames_decoded{0};
        std::atomic<std::uint64_t> decode_last_w{0};
        std::atomic<std::uint64_t> decode_last_h{0};
        ~InboundStream() {
            // Tear down in reverse-construction order: stop QUIC
            // receiving first so no more frames arrive, then let
            // the presenter drain + close its GL context, then
            // the decoder releases surfaces.
            if (quic) quic->Stop();
            presenter.reset();
            decoder.reset();
            if (dump_file) std::fclose(dump_file);
        }
    };
    std::unordered_map<std::string, std::unique_ptr<InboundStream>>
        inbound_;
};

}  // namespace unio
