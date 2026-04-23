#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace unio {

// One decoded frame out of the decoder. The payload is a
// vendor-specific surface handle owned by the decoder — the
// presenter (Day 7 EGL on Linux, Day 6 DXGI flip on Windows)
// dereferences it while the surface ID is still valid and hands
// it back via ReleaseSurface when done.
//
// On Linux the surface handle is a VASurfaceID and native_device
// is a VADisplay; on Windows they're an ID3D11Texture2D* and
// ID3D11Device* respectively. Both wrapped in uintptr_t so this
// header doesn't need to pull in d3d11.h or va/va.h.
struct DecodedFrame {
    std::uintptr_t surface_handle = 0;
    std::uintptr_t native_device = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t decode_done_monotonic_ns = 0;
    // Extracted from the latency SEI the encoder prepends to each
    // slice. Non-zero iff a matching SEI arrived before the slice
    // this frame was decoded from — the sink uses them to compute
    // glass-to-glass when clocks are in sync.
    std::uint64_t frame_id = 0;
    std::uint64_t capture_monotonic_ns = 0;
    bool key_frame = false;
};

// Decoder interface. One concrete implementation per vendor:
// VaapiDecoder on Linux, D3D11VADecoder on Windows (later), plus
// NVDEC on both platforms (Day 8). Kept narrow for the same
// reason as Encoder — vendor SDKs each have their own opinions
// about threading and context binding; those live inside the
// concrete class.
class Decoder {
public:
    using FrameReady = std::function<void(const DecodedFrame&)>;

    virtual ~Decoder() = default;

    struct Config {
        // Decoder surfaces are allocated lazily — the first
        // arrived IDR's SPS tells us picture size. Fps is a hint
        // for rate-control decoupled stats, not a hard constraint.
        int fps = 60;
        // Maximum reference frames to provision in the DPB. Our
        // encoder emits max_num_ref_frames=2 (IPPP with one active
        // reference + headroom); 4 is a safe upper bound for
        // streams we might receive.
        int max_ref_frames = 4;
    };

    // One-shot init. Does not start any threads — decode is
    // driven synchronously by the caller feeding Annex-B packets.
    // on_frame fires on the caller's thread (or the library's
    // worker thread, depending on the vendor) once a frame is
    // fully decoded.
    virtual std::optional<std::string> Init(const Config& cfg,
                                            FrameReady on_frame) = 0;

    // Feed one Annex-B packet. May contain multiple NALs — the
    // decoder scans for start codes and handles each (SPS, PPS,
    // SEI, IDR / non-IDR slice). Returns an error string on
    // unrecoverable decoder death, nullopt otherwise; transient
    // per-packet errors (e.g., missed IDR on connect) are logged
    // and surfaced via helper_status counters rather than
    // interrupting the stream.
    virtual std::optional<std::string> Feed(
        const std::uint8_t* bytes, std::size_t len) = 0;

    virtual std::string_view Name() const = 0;
};

std::unique_ptr<Decoder> MakeVaapiDecoder();
std::unique_ptr<Decoder> MakeD3d11VaDecoder();
// NVDEC on Linux (CUVID / libnvcuvid). Returns nullptr on hosts
// without an NVIDIA driver loaded — the runtime probe picks
// VA-API for Intel / AMD hosts automatically. Parked in PR 9
// as a code-only scaffold; visual validation needs NVIDIA
// Linux hardware we don't have in the default test setup.
std::unique_ptr<Decoder> MakeNvdecDecoder();
std::unique_ptr<Decoder> MakeOneVplDecoder();

}  // namespace unio
