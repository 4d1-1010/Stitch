#pragma once

#include "frame.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace unio {

// One encoded unit out of the encoder. Day-3 shape: Annex-B NALs
// as a flat byte blob with a capture-time stamp for glass-to-
// glass correlation. No explicit NAL boundaries carried —
// downstream demux reads the 00 00 00 01 start codes directly
// off the wire. Matches what the Python pipeline already uses.
struct EncodedPacket {
    std::uint64_t frame_id = 0;          // from CpuFrame::frame_id
    std::uint64_t capture_monotonic_ns = 0;
    std::uint64_t encode_done_monotonic_ns = 0;
    bool key_frame = false;              // IDR marker for subscribers
    std::vector<std::uint8_t> nal_bytes;
};

using EncodedPacketPtr = std::unique_ptr<EncodedPacket>;

// Encoder interface. One concrete implementation per vendor:
// VaapiEncoder on Linux today, NvencEncoder + AmfEncoder +
// OneVplEncoder coming for PR 6 Windows + PR 7. The interface
// is narrow on purpose — each vendor SDK has opinions about
// threading, context binding, frame submission; those live
// inside the concrete classes, not at the dispatch surface.
class Encoder {
public:
    virtual ~Encoder() = default;

    struct Config {
        int width = 0;
        int height = 0;
        int fps = 60;
        int quality = 20;  // CQP-ish; 0 = lossless, 51 = garbage
    };

    // One-shot init. Returns an error string on failure; nullopt
    // on success. Pulling the GPU context can take 50–200 ms, so
    // it happens on the encoder thread, not the capture thread.
    virtual std::optional<std::string> Init(const Config& cfg) = 0;

    // Force the next frame to be an IDR. Called by the control
    // plane on subscribe so the sink's decoder has an anchor.
    virtual void ForceIdr() = 0;

    // Encode one frame. Returns nullptr on encoder failure; the
    // caller treats that as "encoder dead, stop the stream" and
    // surfaces it through helper_status. Per-vendor encoders
    // block (with a tight bound) inside this call — the encode
    // thread is dedicated so blocking is fine.
    virtual EncodedPacketPtr Encode(const CpuFrame& frame) = 0;

    // PR 7 Day 2: zero-copy GPU path. Encoders that can read
    // directly from a captured ID3D11Texture2D override both
    // AcceptsGpu() and EncodeGpu(); the capture side skips the
    // CPU readback when both sides agree. Default = CPU-only,
    // which keeps the Linux XComposite + VA-API path untouched.
    virtual bool AcceptsGpu() const { return false; }
    virtual EncodedPacketPtr EncodeGpu(const GpuFrame& /*frame*/) {
        return nullptr;
    }

    // The shared D3D11 device the encoder uses, exposed so the
    // capture side can build its WinRT Direct3D11CaptureFramePool
    // on the same device (WGC requires the pool to live on the
    // same device as the texture consumer). nullptr on encoders
    // that don't expose a D3D11 device (VA-API, or NVENC before
    // Init). Void* to avoid pulling d3d11.h here.
    virtual void* NativeD3d11Device() const { return nullptr; }

    // Name of the backend for logging / helper_caps.
    virtual std::string_view Name() const = 0;
};

std::unique_ptr<Encoder> MakeVaapiEncoder();
std::unique_ptr<Encoder> MakeNvencEncoder();
// NVENC on Linux (via CUDA input surfaces). Returns nullptr on
// non-Linux builds and on Linux hosts without a working NVIDIA
// driver (libcuda + libnvidia-encode loadable + cuInit+
// cuCtxCreate succeed via CudaRuntime). The Windows NVENC path
// is MakeNvencEncoder above — kept separate because the
// device-type plumbing (D3D11 vs CUDA) is wholly different.
std::unique_ptr<Encoder> MakeNvencLinuxEncoder();
// oneVPL (Intel) H.264 encoder on Windows — libvpl.dll / libmfx.dll.
// Returns nullptr on non-Windows builds and on hosts without
// an Intel graphics driver (or on pre-oneVPL-2.x drivers — the
// filter chain only accepts API 2.x runtimes).
std::unique_ptr<Encoder> MakeOneVplEncoder();

}  // namespace unio
