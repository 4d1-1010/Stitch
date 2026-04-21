// Outbound-stream lifecycle. Day-2 responsibility: spawn the
// capture thread, wire it to an SPSC ring, drain the ring so
// frames actually get consumed. No encoder, no transport. The
// per-stream ``captured`` / ``dropped`` counters surface through
// helper_status so the Python bridge can verify the C++ path is
// alive without waiting for the msquic/NVENC work.

#include "stream_manager.h"

#include <algorithm>
#include <cstdio>

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
    running.store(false);
#if defined(__linux__)
    capture->xc.Close();
#endif
    if (drain_thread.joinable()) drain_thread.join();
}

namespace {

#if defined(__linux__)
void FrameReady(CpuFramePtr frame, void* user) {
    auto* stream = static_cast<OutboundStream*>(user);
    // Handoff to the ring. If the previous slot was still full
    // (consumer behind), ``replace`` returns that frame; we drop
    // it on the floor and bump the stats counter so Python sees
    // the backpressure. Drop-oldest is deliberate: the sink
    // always wants the freshest pixels, not the stalest.
    auto prev = stream->ring.replace(std::move(frame));
    if (prev) {
        stream->dropped.fetch_add(1, std::memory_order_relaxed);
    }
    stream->captured.fetch_add(1, std::memory_order_relaxed);
}
#endif

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
    stream->running.store(true);

#if defined(__linux__)
    if (!stream->capture->xc.Open()) {
        return "XComposite open failed";
    }
    CaptureRect rect{0, 0, width, height};
    (void)monitor_source;  // future: map monitor_id → root offset
    if (!stream->capture->xc.Start(rect, fps, &FrameReady,
                                   stream.get())) {
        return "XComposite start failed";
    }
#else
    (void)monitor_source; (void)fps;
    return "Windows capture not wired yet — PR 6 week 3 work";
#endif

    // Drain thread — Day 2 stand-in for the encoder. Pulls
    // frames out of the ring so the producer can keep writing;
    // the pixels themselves just get dropped for now. Day 3+
    // replaces the drain body with NvEncEncodePicture /
    // vaPutImage + encode.
    auto* raw = stream.get();
    stream->drain_thread = std::thread([raw]() {
        using namespace std::chrono_literals;
        while (raw->running.load(std::memory_order_acquire)) {
            auto frame = raw->ring.pop();
            if (!frame) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            // Sink for this frame: nothing yet. Future work
            // hands the frame to the encoder here.
            (void)frame;
        }
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
    // Destructor joins capture + drain threads.
    to_stop.reset();
    return std::nullopt;
}

std::vector<StreamManager::StatusEntry> StreamManager::Status() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<StatusEntry> out;
    out.reserve(outbound_.size());
    for (const auto& [id, stream] : outbound_) {
        out.push_back(StatusEntry{
            id,
            stream->captured.load(std::memory_order_relaxed),
            stream->dropped.load(std::memory_order_relaxed),
        });
    }
    return out;
}

}  // namespace unio
