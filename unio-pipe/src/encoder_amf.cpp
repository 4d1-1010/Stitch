// AMF H.264 encoder — Windows AMD sibling of encoder_nvenc.cpp
// (NVIDIA) and encoder_onevpl.cpp (Intel). Consumes either the
// BGRA CpuFrame coming out of the WGC capture staging-texture map,
// or the zero-copy GpuFrame carrying an ID3D11Texture2D on the
// shared D3D11 device; emits Annex-B H.264 with a latency-SEI
// prepended for glass-to-glass correlation.
//
// AMF runtime (amfrt64.dll) ships with every AMD graphics driver,
// so this TU dlopens / LoadLibrary's it rather than linking at
// build time — the same runtime-loaded-from-vendor-DLL pattern
// we already use for nvEncodeAPI64.dll. No AMF redistributable
// from us. The SDK itself (GPUOpen/AMF, SPDX MIT) contributes
// headers only; build has no link dep.
//
// Scope matches the nvenc + onevpl paths: Main-profile H.264 for
// the widest decode denominator across the other three sinks
// (D3D11VA / NVDEC / oneVPL), CQP-style rate control, zero-copy
// D3D11 input when the capture side can share our device. The
// latency-SEI is prepended exactly the same way the NVENC path
// does it — the decoder matches regardless of which encoder
// produced the bitstream.
//
// Validation status: cross-compile-verified under the Docker
// msvc-wine toolchain; hardware-verified ON AMD HARDWARE still
// pending — see issue #25. No runtime test in the matrix runner
// until the first hardware pass lands.

#if !defined(_WIN32)
#include "encoder.h"
namespace unio {
std::unique_ptr<Encoder> MakeAmfEncoder() { return nullptr; }
}  // namespace unio
#else

#include "encoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

// AMF SDK (GPUOpen) — headers only, runtime is amfrt64.dll.
// AMF's public C++ wrapper uses reference-counted smart pointers
// (AMFContextPtr / AMFComponentPtr / AMFSurfacePtr / AMFDataPtr)
// that hold raw AMF C-API objects and call Release() on scope
// exit — matches ComPtr<> shape closely.
#include <core/Factory.h>
#include <core/Context.h>
#include <core/Surface.h>
#include <core/Buffer.h>
#include <core/Data.h>
#include <core/Result.h>
#include <components/VideoEncoderVCE.h>
#include <components/Component.h>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count());
}

// AMF's entry-point signature. Resolved at runtime from amfrt64.dll.
// Matches AMF_CORE_VERSION pinned from the headers at build time;
// if the installed driver's runtime is older than our headers,
// AMFInit returns AMF_VERSION_MISMATCH and we surface that.
using PFN_AMFInit = AMF_RESULT (AMF_CDECL_CALL*)(
    amf_uint64 version, amf::AMFFactory** ppFactory);

class AmfEncoder final : public Encoder {
public:
    AmfEncoder() = default;
    ~AmfEncoder() override { Teardown(); }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;
        if (auto err = LoadDll(); err) return err;

        // Enumerate DXGI adapters and pick the AMD one explicitly.
        // On hybrid AMD + Intel boxes D3D11_DRIVER_TYPE_HARDWARE
        // lands on whichever adapter Windows prefers, which tends
        // toward the Intel iGPU — same problem the NVENC path
        // solves by hunting for VendorId 0x10DE. We hunt for
        // 0x1002 (AMD/ATI). AMF requires a D3D11 device from the
        // AMD adapter so the encoder can access the VCE / VCN
        // hardware pipeline directly.
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return "CreateDXGIFactory1 failed";
        }
        ComPtr<IDXGIAdapter1> chosen;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.VendorId == 0x1002) {  // AMD/ATI
                chosen = a;
                break;
            }
        }
        if (!chosen) {
            return "no AMD GPU found (vendor 0x1002)";
        }

        UINT flags = 0;
        D3D_FEATURE_LEVEL fl = {};
        if (FAILED(D3D11CreateDevice(
                chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                flags, nullptr, 0, D3D11_SDK_VERSION,
                &device_, &fl, &ctx_))) {
            return "D3D11CreateDevice on AMD adapter failed";
        }
        // AMF accesses the D3D11 device from its worker threads;
        // flip the multithread-protection bit the same way
        // decoder_d3d11va.cpp does.
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device_.As(&mt))) {
            mt->SetMultithreadProtected(TRUE);
        }

        AMF_RESULT ar = amf_init_(AMF_FULL_VERSION, &factory_);
        if (ar != AMF_OK || !factory_) {
            return "AMFInit failed (rc=" + std::to_string(ar) + ")";
        }
        ar = factory_->CreateContext(&context_);
        if (ar != AMF_OK || !context_) {
            return "AMFFactory::CreateContext failed (rc="
                   + std::to_string(ar) + ")";
        }
        ar = context_->InitDX11(device_.Get(), amf::AMF_DX11_1);
        if (ar != AMF_OK) {
            return "AMFContext::InitDX11 failed (rc="
                   + std::to_string(ar) + ")";
        }

        ar = factory_->CreateComponent(context_,
                                        AMFVideoEncoderVCE_AVC,
                                        &encoder_);
        if (ar != AMF_OK || !encoder_) {
            return "AMFFactory::CreateComponent(VCE_AVC) failed (rc="
                   + std::to_string(ar) + ")";
        }

        // Encoder properties — mirror of the NVENC LOW_LATENCY +
        // CQP + infinite-GOP configuration from encoder_nvenc.cpp.
        // AMF's property name strings are the C-API "string enum"
        // pattern used throughout the SDK (AMF_VIDEO_ENCODER_*).
        //
        // USAGE=LOWEST_LATENCY picks the VCE preset bundle that
        // disables B-frames, lookahead, and adaptive QP — the same
        // knobs we explicitly turn off on the NVENC side.
        if ((ar = encoder_->SetProperty(
                AMF_VIDEO_ENCODER_USAGE,
                AMF_VIDEO_ENCODER_USAGE_LOW_LATENCY)) != AMF_OK) {
            return "SetProperty USAGE failed (rc="
                   + std::to_string(ar) + ")";
        }
        // Main profile, same as NVENC and oneVPL sides.
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_PROFILE,
            AMF_VIDEO_ENCODER_PROFILE_MAIN);
        // Quality preset: SPEED trades BD-rate for encode-time —
        // acceptable for live capture, matches NVENC's P4 intent.
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_QUALITY_PRESET,
            AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
        // CQP. AMF's CQP mode exposes separate I/P QPs (no B).
        // Same QP mapping as NVENC: drop a few steps below the
        // requested quality to compensate for generation loss on
        // already-compressed Windows-desktop content.
        const int qp = std::max(cfg.quality - 5, 10);
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
            AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP);
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_QP_I, qp);
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_QP_P, qp);
        // Frame rate. AMF encodes the fraction as AMFRate
        // (num/den) — our cfg always carries an integer fps so
        // den=1 always.
        AMFRate rate = {static_cast<amf_uint32>(cfg.fps), 1};
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_FRAMERATE, rate);
        // IDR cadence: control plane drives keyframes via
        // ForceIdr(). Disable any periodic IDR the preset might
        // enable (same as NVENC gopLength = INFINITE).
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_IDR_PERIOD, 0);
        // SPS/PPS on every IDR so decoders can resync without
        // needing out-of-band parameter sets.
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, 0);
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_INSERT_SPS, true);
        (void)encoder_->SetProperty(
            AMF_VIDEO_ENCODER_INSERT_PPS, true);

        ar = encoder_->Init(
            amf::AMF_SURFACE_BGRA,
            cfg.width, cfg.height);
        if (ar != AMF_OK) {
            return "AMFComponent::Init failed (rc="
                   + std::to_string(ar) + ")";
        }

        std::fprintf(stderr,
            "unio-pipe: AMF encoder initialized %dx%d@%dfps, "
            "H.264 Main, CQP %d\n",
            cfg.width, cfg.height, cfg.fps, qp);
        initialized_ = true;
        return std::nullopt;
    }

    void ForceIdr() override {
        force_idr_.store(true, std::memory_order_release);
    }

    EncodedPacketPtr Encode(const CpuFrame& frame) override {
        if (!initialized_) return nullptr;

        // Allocate a host-visible BGRA surface, copy in the
        // captured pixels, then submit. AMF handles the
        // host-to-device copy internally via AllocSurface on the
        // MEMORY_HOST type.
        amf::AMFSurfacePtr surf;
        if (context_->AllocSurface(
                amf::AMF_MEMORY_HOST, amf::AMF_SURFACE_BGRA,
                cfg_.width, cfg_.height, &surf) != AMF_OK
            || !surf) {
            return nullptr;
        }
        amf::AMFPlane* plane = surf->GetPlaneAt(0);
        if (!plane) return nullptr;
        auto* dst = static_cast<std::uint8_t*>(plane->GetNative());
        const std::int32_t dst_pitch = plane->GetHPitch();
        const std::uint32_t src_stride = frame.stride_bytes;
        const std::uint32_t rows = std::min<std::uint32_t>(
            frame.height, static_cast<std::uint32_t>(cfg_.height));
        const std::uint32_t row_bytes = std::min<std::uint32_t>(
            src_stride, static_cast<std::uint32_t>(dst_pitch));
        const auto* src = frame.pixels.data();
        for (std::uint32_t y = 0; y < rows; ++y) {
            std::memcpy(dst + y * dst_pitch,
                        src + y * src_stride, row_bytes);
        }
        return SubmitAndDrain(surf.GetPtr(),
                               frame.frame_id,
                               frame.capture_monotonic_ns);
    }

    bool AcceptsGpu() const override { return true; }

    void* NativeD3d11Device() const override {
        return device_.Get();
    }

    EncodedPacketPtr EncodeGpu(const GpuFrame& frame) override {
        if (!initialized_ || !frame.native_texture) return nullptr;
        auto* tex = static_cast<ID3D11Texture2D*>(frame.native_texture);

        // Wrap the existing D3D11 texture in an AMFSurface without
        // copying. CreateSurfaceFromDX11Native hands ownership-
        // sharing semantics: AMF adds a reference on the texture
        // and releases it when the AMFSurface's refcount hits zero.
        // The WGC capture pool owns the original reference, so our
        // AMFSurfacePtr going out of scope is safe.
        amf::AMFSurfacePtr surf;
        if (context_->CreateSurfaceFromDX11Native(
                tex, &surf, nullptr) != AMF_OK || !surf) {
            return nullptr;
        }
        return SubmitAndDrain(surf.GetPtr(),
                               frame.frame_id,
                               frame.capture_monotonic_ns);
    }

    std::string_view Name() const override { return "amf"; }

private:
    EncodedPacketPtr SubmitAndDrain(amf::AMFSurface* surf,
                                     std::uint64_t frame_id,
                                     std::uint64_t capture_ns) {
        const bool is_idr = force_idr_.exchange(false,
                std::memory_order_acq_rel);
        if (is_idr) {
            // AMF per-frame IDR request. Cleared automatically
            // after one frame — same semantics as NVENC's
            // NV_ENC_PIC_FLAG_FORCEIDR.
            (void)surf->SetProperty(
                AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
                AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            (void)surf->SetProperty(
                AMF_VIDEO_ENCODER_INSERT_SPS, true);
            (void)surf->SetProperty(
                AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
        // Thread through our capture timestamp as an AMFSurface
        // property so we can recover it on the output data — AMF
        // propagates user properties from input surface to output
        // buffer automatically. Simpler than keeping a side map.
        (void)surf->SetProperty(L"unio_capture_ns",
                                 static_cast<amf_int64>(capture_ns));
        (void)surf->SetProperty(L"unio_frame_id",
                                 static_cast<amf_int64>(frame_id));

        AMF_RESULT ar = encoder_->SubmitInput(surf);
        if (ar == AMF_INPUT_FULL) {
            // The encoder's input queue is full — drain at least
            // one output to make room, then retry once. Matches
            // NVENC's NEED_MORE_INPUT handling but in the
            // opposite direction (NVENC tells us it needs more,
            // AMF tells us it can't take more).
            if (auto pkt = DrainOnce(frame_id, capture_ns); pkt) {
                // Return the drained packet; the caller's next
                // Encode() call will retry the unsubmitted frame.
                // Live capture is single-frame-at-a-time so this
                // path is exercised only under instantaneous load
                // spikes.
                return pkt;
            }
            ar = encoder_->SubmitInput(surf);
        }
        if (ar != AMF_OK) {
            std::fprintf(stderr,
                "unio-pipe: AMF SubmitInput failed rc=%d\n",
                static_cast<int>(ar));
            return nullptr;
        }
        return DrainOnce(frame_id, capture_ns);
    }

    EncodedPacketPtr DrainOnce(std::uint64_t fallback_frame_id,
                                std::uint64_t fallback_capture_ns) {
        amf::AMFDataPtr out;
        AMF_RESULT ar = encoder_->QueryOutput(&out);
        if (ar == AMF_REPEAT || ar == AMF_NEED_MORE_INPUT) {
            // Encoder has accepted the frame but not yet emitted a
            // bitstream (VCE's pipeline depth is typically 1-2
            // frames). Hand the caller an empty marker packet so
            // stream_manager advances the frame accounting without
            // dropping the capture timestamp on the floor.
            auto skip = std::make_unique<EncodedPacket>();
            skip->frame_id = fallback_frame_id;
            skip->capture_monotonic_ns = fallback_capture_ns;
            return skip;
        }
        if (ar != AMF_OK || !out) {
            std::fprintf(stderr,
                "unio-pipe: AMF QueryOutput failed rc=%d\n",
                static_cast<int>(ar));
            return nullptr;
        }

        // Recover the per-frame capture timestamp we stashed on
        // the input surface. AMF copies user properties through
        // to the output data.
        amf_int64 cap_ns = static_cast<amf_int64>(fallback_capture_ns);
        amf_int64 fid = static_cast<amf_int64>(fallback_frame_id);
        (void)out->GetProperty(L"unio_capture_ns", &cap_ns);
        (void)out->GetProperty(L"unio_frame_id", &fid);

        amf::AMFBufferPtr buf(out);
        if (!buf) return nullptr;
        const auto* bytes = static_cast<const std::uint8_t*>(
            buf->GetNative());
        const std::size_t len = buf->GetSize();
        if (!bytes || len == 0) return nullptr;

        // AMF's VCE encoder emits Annex-B by default (00 00 00 01
        // start codes, SPS/PPS inlined before IDRs when we asked
        // via AMF_VIDEO_ENCODER_INSERT_SPS/PPS). Matches the
        // NVENC Annex-B output — no bitstream-format conversion
        // needed before hitting the wire.

        // Pick out the picture type from the first VCL NAL so
        // we can set key_frame correctly. NALs start after the
        // 00 00 00 01 or 00 00 01 prefix.
        bool is_idr = false;
        {
            std::size_t i = 0;
            while (i + 4 < len) {
                if (bytes[i] == 0 && bytes[i+1] == 0) {
                    std::size_t hdr_off;
                    if (bytes[i+2] == 1) hdr_off = i + 3;
                    else if (bytes[i+2] == 0 && bytes[i+3] == 1) hdr_off = i + 4;
                    else { ++i; continue; }
                    if (hdr_off >= len) break;
                    const std::uint8_t nal_type = bytes[hdr_off] & 0x1F;
                    if (nal_type == 5) { is_idr = true; break; }
                    if (nal_type == 1) break;  // non-IDR slice
                    i = hdr_off + 1;
                    continue;
                }
                ++i;
            }
        }

        auto pkt = std::make_unique<EncodedPacket>();
        pkt->frame_id = static_cast<std::uint64_t>(fid);
        pkt->capture_monotonic_ns = static_cast<std::uint64_t>(cap_ns);
        pkt->key_frame = is_idr;
        auto sei = BuildLatencySeiAnnexB(
            pkt->frame_id, pkt->capture_monotonic_ns);
        pkt->nal_bytes.reserve(sei.size() + len);
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                              sei.begin(), sei.end());
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                              bytes, bytes + len);
        pkt->encode_done_monotonic_ns = NowNs();
        ++frame_count_;
        if (frame_count_ <= 3 || is_idr) {
            std::fprintf(stderr,
                "unio-pipe: amf encode frame %llu idr=%d len=%zu\n",
                static_cast<unsigned long long>(frame_count_),
                is_idr ? 1 : 0, len);
            std::fflush(stderr);
        }
        return pkt;
    }

    std::optional<std::string> LoadDll() {
        dll_ = ::LoadLibraryA("amfrt64.dll");
        if (!dll_) {
            return "amfrt64.dll not found (install AMD driver)";
        }
        amf_init_ = reinterpret_cast<PFN_AMFInit>(
            ::GetProcAddress(dll_, AMF_INIT_FUNCTION_NAME));
        if (!amf_init_) {
            return std::string(AMF_INIT_FUNCTION_NAME)
                   + " not in amfrt64.dll (driver too old?)";
        }
        return std::nullopt;
    }

    void Teardown() {
        if (encoder_) {
            encoder_->Terminate();
            encoder_ = nullptr;
        }
        if (context_) {
            context_->Terminate();
            context_ = nullptr;
        }
        factory_ = nullptr;  // factory is owned by amfrt64; no Release
        ctx_.Reset();
        device_.Reset();
        // Intentionally do NOT FreeLibrary(dll_). Same reason as
        // NVENC: AMF spawns internal worker threads and callbacks
        // that can fire after Terminate() returns; tearing the DLL
        // down on stop-restart cycles races those threads and lands
        // as a wild jump into freed code pages.
        initialized_ = false;
    }

    Config cfg_{};
    HMODULE dll_ = nullptr;
    PFN_AMFInit amf_init_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;   // owned by amfrt64
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr encoder_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    bool initialized_ = false;
    std::atomic<bool> force_idr_{true};
    std::uint64_t frame_count_ = 0;
};

}  // namespace

std::unique_ptr<Encoder> MakeAmfEncoder() {
    return std::make_unique<AmfEncoder>();
}

}  // namespace unio

#endif  // _WIN32
