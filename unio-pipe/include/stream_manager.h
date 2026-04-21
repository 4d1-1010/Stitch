#pragma once

#include "frame.h"
#include "spsc_ring.h"
#include "unio_pipe.h"

#include <atomic>
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
    std::atomic<std::uint64_t> dropped{0};
    SpscRing<CpuFrame, 2> ring;
    std::thread drain_thread;

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
    // Returns nullopt on success, otherwise an error string.
    std::optional<std::string> StartOutbound(
        std::string_view stream_id,
        std::string_view monitor_source,
        int width, int height, int fps);

    std::optional<std::string> Stop(std::string_view stream_id);

    // helper_status payload: one entry per live stream.
    struct StatusEntry {
        std::string stream_id;
        std::uint64_t frames_captured = 0;
        std::uint64_t frames_dropped = 0;
    };
    std::vector<StatusEntry> Status() const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<OutboundStream>>
        outbound_;
};

}  // namespace unio
