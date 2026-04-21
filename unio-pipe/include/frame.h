#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace unio {

// A captured frame in CPU memory. Windows path eventually hands
// the encoder an ID3D11Texture2D instead and skips this struct
// entirely; on Linux the XComposite readback is host memory by
// definition (XShm SHM segment), so we round-trip a BGRX buffer
// here.
//
// Fields are intentionally public — this is a plain data carrier
// owned by an SPSC ring slot, not an encapsulated type.
struct CpuFrame {
    // Pixel layout: BGRX 32 bpp (B, G, R, pad), little-endian.
    // Matches what XShmGetImage returns on a TrueColor root
    // visual and what the XComposite capture already produces
    // on the Python side.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride_bytes = 0;   // usually width*4; padding allowed

    // The capture thread's monotonic frame_id. Propagates through
    // the pipeline so encoder / transport / sink can correlate
    // with the latency_trace CSVs without a per-stage counter.
    std::uint64_t frame_id = 0;

    // Absolute monotonic-ns timestamp of capture completion.
    // Sink side reads it from the NAL header after decode so
    // glass-to-glass is directly measurable when clocks are in
    // sync (same machine, or NTP-disciplined peers).
    std::uint64_t capture_monotonic_ns = 0;

    std::vector<std::uint8_t> pixels;  // size = stride_bytes * height
};

using CpuFramePtr = std::unique_ptr<CpuFrame>;

}  // namespace unio
