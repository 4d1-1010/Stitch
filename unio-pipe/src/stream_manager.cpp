// Outbound-stream lifecycle. Three threads per stream:
//
//   capture  ───► frame_ring (depth 2)   ───► encode
//   encode   ───► packet_ring (depth 4)  ───► send
//   send     ───► (Day 4: msquic; today: counters only)
//
// Drop-oldest at both ring boundaries so a stall anywhere
// downstream never blocks the capture thread. helper_status
// exposes the captured / dropped / encoded / bytes counters so
// the Python side (and CI) can verify the whole producer chain
// end-to-end without msquic being wired.

#include "stream_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

#if defined(__linux__)
#include "capture_xcomposite.h"
#elif defined(_WIN32)
#include "capture_wgc.h"
#endif

namespace unio {

struct OutboundStream::Capture {
#if defined(__linux__)
    XCompositeCapture xc;
#elif defined(_WIN32)
    WgcCapture wgc;
#endif
};

OutboundStream::OutboundStream()
    : capture(std::make_unique<Capture>()) {}

OutboundStream::~OutboundStream() {
    running.store(false, std::memory_order_release);
#if defined(__linux__)
    capture->xc.Close();
#elif defined(_WIN32)
    capture->wgc.Close();
#endif
    if (encode_thread.joinable()) encode_thread.join();
    if (send_thread.joinable()) send_thread.join();
    if (dump_file) {
        std::fclose(dump_file);
        dump_file = nullptr;
    }
}

namespace {

#if defined(__linux__) || defined(_WIN32)
void FrameReady(CpuFramePtr frame, void* user) {
    auto* stream = static_cast<OutboundStream*>(user);
    // Handoff to the frame ring. Drop-oldest if the encoder is
    // behind — the sink always wants the freshest pixels.
    auto prev = stream->frame_ring.replace(std::move(frame));
    if (prev) {
        stream->dropped_at_ring.fetch_add(
            1, std::memory_order_relaxed);
    }
    stream->captured.fetch_add(1, std::memory_order_relaxed);
}
#endif

void EncodeLoop(OutboundStream* stream) {
    using namespace std::chrono_literals;
    while (stream->running.load(std::memory_order_acquire)) {
        auto frame = stream->frame_ring.pop();
        if (!frame) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (!stream->encoder) continue;
        auto pkt = stream->encoder->Encode(*frame);
        if (!pkt) {
            // Encoder death — break the loop and let Status()
            // surface the encoded=0 stall to Python.
            stream->running.store(false, std::memory_order_release);
            break;
        }
        stream->encoded.fetch_add(1, std::memory_order_relaxed);
        auto prev = stream->packet_ring.replace(std::move(pkt));
        if (prev) {
            stream->dropped_at_send.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

void SendLoop(OutboundStream* stream) {
    // Pop encoded packets, write to QUIC if connected, mirror to
    // the bitstream dump if configured, and count bytes. The QUIC
    // handle is optional — when StartOutbound was called with an
    // empty peer_host we stay dump-only, which is how the CI
    // loopback test exercises the capture+encode path without
    // network I/O.
    using namespace std::chrono_literals;
    while (stream->running.load(std::memory_order_acquire)) {
        auto pkt = stream->packet_ring.pop();
        if (!pkt) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        stream->bytes_emitted.fetch_add(
            pkt->nal_bytes.size(), std::memory_order_relaxed);
        if (stream->dump_file && !pkt->nal_bytes.empty()) {
            std::fwrite(pkt->nal_bytes.data(), 1,
                        pkt->nal_bytes.size(), stream->dump_file);
        }
        if (stream->quic && !pkt->nal_bytes.empty()) {
            if (!stream->quic->SendPacket(pkt->nal_bytes.data(),
                                          pkt->nal_bytes.size())) {
                // QUIC went away — drop future packets quietly;
                // helper_status will show quic_connected=false
                // and bytes_emitted flat-lining.
                stream->quic.reset();
            }
        }
    }
    if (stream->dump_file) {
        std::fflush(stream->dump_file);
    }
}

}  // namespace

StreamManager::StreamManager() = default;
StreamManager::~StreamManager() {
    std::lock_guard<std::mutex> lk(mu_);
    outbound_.clear();
}

std::optional<std::string> StreamManager::StartOutbound(
        std::string_view stream_id,
        std::string_view monitor_source,
        std::string_view peer_host,
        int peer_port,
        int width, int height, int fps,
        int capture_x, int capture_y) {
    if (stream_id.empty()) return "missing stream_id";
    if (width <= 0 || height <= 0) return "bad rect";

    std::lock_guard<std::mutex> lk(mu_);
    std::string key(stream_id);
    if (outbound_.find(key) != outbound_.end()) {
        return "stream already running";
    }
    auto stream = std::make_unique<OutboundStream>();
    stream->stream_id = key;

    if (const char* dump_path = std::getenv("UNIO_PIPE_BITSTREAM_DUMP")) {
        if (dump_path[0] != '\0') {
            stream->dump_file = std::fopen(dump_path, "wb");
            if (stream->dump_file) {
                std::fprintf(stderr,
                             "unio-pipe: dumping bitstream for %s "
                             "to %s\n", key.c_str(), dump_path);
            }
        }
    }

    // Optional QUIC outbound. If peer_host is empty the helper
    // runs as dump-only (tests, offline captures) — otherwise
    // connect now, blocking briefly until the handshake completes
    // so a failure here surfaces in the start_outbound response
    // instead of as silent packet loss later.
    if (!peer_host.empty() && peer_port > 0) {
        stream->quic = std::make_unique<QuicOutbound>();
        QuicOutbound::Config qc;
        qc.peer_host = std::string(peer_host);
        qc.peer_port = static_cast<std::uint16_t>(peer_port);
        qc.stream_id = key;
        if (auto err = stream->quic->Connect(qc); err) {
            return "quic connect: " + *err;
        }
        std::fprintf(stderr,
                     "unio-pipe: quic outbound %s → %s:%d connected\n",
                     key.c_str(), qc.peer_host.c_str(), qc.peer_port);
    }

#if defined(__linux__)
    stream->encoder = MakeVaapiEncoder();
    if (!stream->encoder) {
        return "no encoder (VA-API factory returned null)";
    }
    Encoder::Config ec{width, height, fps, /*quality=*/20};
    if (auto err = stream->encoder->Init(ec); err) {
        return "encoder init: " + *err;
    }
    stream->running.store(true);

    if (!stream->capture->xc.Open()) {
        return "XComposite open failed";
    }
    CaptureRect rect{capture_x, capture_y, width, height};
    (void)monitor_source;
    if (!stream->capture->xc.Start(rect, fps, &FrameReady,
                                   stream.get())) {
        return "XComposite start failed";
    }
#elif defined(_WIN32)
    (void)monitor_source;
    stream->encoder = MakeNvencEncoder();
    if (!stream->encoder) {
        return "no encoder (NVENC factory returned null)";
    }
    Encoder::Config ec{width, height, fps, /*quality=*/20};
    if (auto err = stream->encoder->Init(ec); err) {
        return "encoder init: " + *err;
    }
    if (!stream->capture->wgc.Open()) {
        return "WGC open failed (requires Win10 1903+)";
    }
    WgcRect rect{capture_x, capture_y, width, height};
    if (!stream->capture->wgc.Start(rect, fps, &FrameReady,
                                     stream.get())) {
        return "WGC start failed";
    }
    stream->running.store(true);
#else
    (void)monitor_source; (void)fps;
    (void)capture_x; (void)capture_y;
    return "capture not wired yet on this platform";
#endif

    auto* raw = stream.get();
    stream->encode_thread = std::thread([raw]() {
        EncodeLoop(raw);
    });
    stream->send_thread = std::thread([raw]() {
        SendLoop(raw);
    });

    outbound_.emplace(key, std::move(stream));
    return std::nullopt;
}

std::optional<std::string> StreamManager::StartInbound(
        std::string_view stream_id, int port,
        int window_w, int window_h) {
    if (stream_id.empty()) return "missing stream_id";
    if (port <= 0 || port > 65535) return "bad port";
    std::lock_guard<std::mutex> lk(mu_);
    std::string key(stream_id);
    if (inbound_.find(key) != inbound_.end()) {
        return "stream already running";
    }
    auto stream = std::make_unique<InboundStream>();
    stream->stream_id = key;
    if (const char* dump_path = std::getenv("UNIO_PIPE_BITSTREAM_DUMP")) {
        if (dump_path[0] != '\0') {
            stream->dump_file = std::fopen(dump_path, "wb");
            if (stream->dump_file) {
                std::fprintf(stderr,
                    "unio-pipe: inbound %s dumping bitstream to %s\n",
                    key.c_str(), dump_path);
            }
        }
    }
    auto* raw = stream.get();

    // Presenter first — if the display isn't available (headless
    // CI, no DISPLAY) we silently run without it and still count
    // frames_decoded. This keeps the inbound lifecycle bringing-
    // up-friendly: one failure mode doesn't kill the rest.
    // OS dispatch: DXGI flip on Windows, EGL/X11 on Linux.
#if defined(_WIN32)
    stream->presenter = MakeDxgiFlipPresenter();
#else
    stream->presenter = MakeEglX11Presenter();
#endif
    if (stream->presenter) {
        Presenter::Config pc;
        pc.width = window_w;
        pc.height = window_h;
        pc.window_title = "unio-pipe sink: " + key;
        if (auto perr = stream->presenter->Init(pc); perr) {
            std::fprintf(stderr,
                "unio-pipe: presenter init failed (%s) — "
                "inbound runs headless\n", perr->c_str());
            stream->presenter.reset();
        }
    }

#if defined(_WIN32)
    stream->decoder = MakeD3d11VaDecoder();
#else
    stream->decoder = MakeVaapiDecoder();
#endif
    if (stream->decoder) {
        Decoder::Config dc;
        auto derr = stream->decoder->Init(dc,
            [raw](const DecodedFrame& df) {
                raw->frames_decoded.fetch_add(
                    1, std::memory_order_relaxed);
                raw->decode_last_w.store(df.width,
                    std::memory_order_relaxed);
                raw->decode_last_h.store(df.height,
                    std::memory_order_relaxed);
                if (raw->presenter) raw->presenter->Present(df);
            });
        if (derr) {
            std::fprintf(stderr,
                "unio-pipe: decoder init failed (%s) — "
                "inbound runs as dump-only\n", derr->c_str());
            stream->decoder.reset();
        }
    }

    stream->quic = std::make_unique<QuicInbound>();
    QuicInbound::Config qc;
    qc.listen_port = static_cast<std::uint16_t>(port);
    qc.stream_id = key;

    // The callback runs on an msquic worker thread; keep it fast.
    // Writing to the optional dump file is a plain fwrite, and the
    // decoder's Feed() drives VA-API synchronously which is fine
    // at the 30-60 fps rates we ship at — a single IDR at 1080p
    // decodes in ~1 ms on Intel UHD 630. If that ever becomes a
    // bottleneck we'll hand off to a dedicated decode thread.
    auto err = stream->quic->Start(qc, [raw](const std::uint8_t* b,
                                             std::size_t n) {
        if (raw->dump_file && n > 0) {
            std::fwrite(b, 1, n, raw->dump_file);
            std::fflush(raw->dump_file);
        }
        raw->bytes_written.fetch_add(n, std::memory_order_relaxed);
        if (raw->decoder) {
            raw->decoder->Feed(b, n);
        }
    });
    if (err) return "quic inbound: " + *err;
    std::fprintf(stderr,
        "unio-pipe: quic inbound %s listening on port %d\n",
        key.c_str(), port);
    inbound_.emplace(key, std::move(stream));
    return std::nullopt;
}

std::optional<std::string> StreamManager::RequestIdr(
        std::string_view stream_id) {
    std::string key(stream_id);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = outbound_.find(key);
    if (it == outbound_.end()) return "no such outbound stream";
    if (it->second->encoder) {
        it->second->encoder->ForceIdr();
    }
    return std::nullopt;
}

std::optional<std::string> StreamManager::Stop(
        std::string_view stream_id) {
    std::string key(stream_id);
    {
        std::unique_ptr<InboundStream> in_to_stop;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = inbound_.find(key);
            if (it != inbound_.end()) {
                in_to_stop = std::move(it->second);
                inbound_.erase(it);
            }
        }
        if (in_to_stop) {
            in_to_stop.reset();
            return std::nullopt;
        }
    }
    std::unique_ptr<OutboundStream> to_stop;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = outbound_.find(key);
        if (it == outbound_.end()) return "no such stream";
        to_stop = std::move(it->second);
        outbound_.erase(it);
    }
    // Destructor joins capture + encode + send threads.
    to_stop.reset();
    return std::nullopt;
}

std::vector<StreamManager::StatusEntry> StreamManager::Status() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<StatusEntry> out;
    out.reserve(outbound_.size() + inbound_.size());
    for (const auto& [id, stream] : outbound_) {
        StatusEntry e;
        e.stream_id = id;
        e.direction = "outbound";
        e.frames_captured =
            stream->captured.load(std::memory_order_relaxed);
        e.frames_dropped_at_ring =
            stream->dropped_at_ring.load(std::memory_order_relaxed);
        e.frames_encoded =
            stream->encoded.load(std::memory_order_relaxed);
        e.packets_dropped_at_send =
            stream->dropped_at_send.load(std::memory_order_relaxed);
        e.bytes_emitted =
            stream->bytes_emitted.load(std::memory_order_relaxed);
        if (stream->encoder) {
            e.encoder = std::string(stream->encoder->Name());
        }
        e.quic_connected = stream->quic && stream->quic->IsConnected();
        out.push_back(std::move(e));
    }
    for (const auto& [id, stream] : inbound_) {
        StatusEntry e;
        e.stream_id = id;
        e.direction = "inbound";
        if (stream->quic) {
            e.packets_received = stream->quic->PacketsReceived();
            e.bytes_received = stream->quic->BytesReceived();
        }
        e.frames_decoded =
            stream->frames_decoded.load(std::memory_order_relaxed);
        e.decode_width = static_cast<std::uint32_t>(
            stream->decode_last_w.load(std::memory_order_relaxed));
        e.decode_height = static_cast<std::uint32_t>(
            stream->decode_last_h.load(std::memory_order_relaxed));
        if (stream->decoder) {
            e.decoder = std::string(stream->decoder->Name());
        }
        if (stream->presenter) {
            e.presenter = std::string(stream->presenter->Name());
            e.frames_presented = stream->presenter->FramesPresented();
        }
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace unio
