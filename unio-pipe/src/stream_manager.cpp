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
#include <thread>

#if defined(__linux__)
#include "capture_xcomposite.h"
#endif

namespace unio {

struct OutboundStream::Capture {
#if defined(__linux__)
    XCompositeCapture xc;
#endif
};

OutboundStream::OutboundStream()
    : capture(std::make_unique<Capture>()) {}

OutboundStream::~OutboundStream() {
    running.store(false, std::memory_order_release);
#if defined(__linux__)
    capture->xc.Close();
#endif
    if (encode_thread.joinable()) encode_thread.join();
    if (send_thread.joinable()) send_thread.join();
}

namespace {

#if defined(__linux__)
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
    // Day-4 work (msquic) lives here. Day 3 just pops the packet
    // and accounts for its byte count — the bytes-emitted number
    // is a useful "is the whole chain alive" signal and also
    // what ``helper_status`` surfaces.
    using namespace std::chrono_literals;
    while (stream->running.load(std::memory_order_acquire)) {
        auto pkt = stream->packet_ring.pop();
        if (!pkt) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        stream->bytes_emitted.fetch_add(
            pkt->nal_bytes.size(), std::memory_order_relaxed);
        // TODO(pr6-week2): msquic stream write goes here.
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
        int width, int height, int fps) {
    if (stream_id.empty()) return "missing stream_id";
    if (width <= 0 || height <= 0) return "bad rect";

    std::lock_guard<std::mutex> lk(mu_);
    std::string key(stream_id);
    if (outbound_.find(key) != outbound_.end()) {
        return "stream already running";
    }
    auto stream = std::make_unique<OutboundStream>();
    stream->stream_id = key;

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
    CaptureRect rect{0, 0, width, height};
    (void)monitor_source;
    if (!stream->capture->xc.Start(rect, fps, &FrameReady,
                                   stream.get())) {
        return "XComposite start failed";
    }
#else
    (void)monitor_source; (void)fps;
    return "Windows capture not wired yet — PR 6 week 3";
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

std::optional<std::string> StreamManager::Stop(
        std::string_view stream_id) {
    std::unique_ptr<OutboundStream> to_stop;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = outbound_.find(std::string(stream_id));
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
    out.reserve(outbound_.size());
    for (const auto& [id, stream] : outbound_) {
        StatusEntry e;
        e.stream_id = id;
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
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace unio
