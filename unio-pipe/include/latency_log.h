#pragma once

#include <cstdint>
#include <string_view>

// Per-frame latency CSV emitter used by both presenters
// (EGL/X11 on Linux, DXGI flip on Windows). When the environment
// variable named by `env_var` is set to a non-empty path at
// first-call time, LogLatency opens that path in write mode and
// writes one row per decoded frame. If the env var is unset, the
// call is a no-op and cheaper than taking a timestamp.
//
// Timestamps are absolute monotonic nanoseconds. The reader
// (`tools/plot_latency.py`, or whatever a human writes) subtracts
// capture_ns from the later stages to get per-stage latencies.
// Every row also carries width × height so a capture-size change
// mid-stream is visible in the log.
//
// Thread-safe: an internal mutex serialises the fprintf so two
// presenter threads emitting concurrently don't interleave bytes.

namespace unio {

void LogLatency(std::string_view env_var,
                std::uint64_t frame_id,
                std::uint64_t capture_monotonic_ns,
                std::uint64_t decode_done_monotonic_ns,
                std::uint64_t present_done_monotonic_ns,
                std::uint32_t width,
                std::uint32_t height);

}  // namespace unio
