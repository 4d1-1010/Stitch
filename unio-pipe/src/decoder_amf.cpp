// AMF H.264 decoder — Windows AMD sibling of decoder_d3d11va.cpp
// (generic DXVA2) and decoder_onevpl.cpp (Intel). Annex-B packets
// arrive via Feed(); amfrt64.dll's UVD decode pipeline hands back
// an AMFSurface backed by a D3D11 NV12 texture that the DXGI flip
// presenter can sample directly.
//
// Same DLL as encoder_amf.cpp (the OS refcounts HMODULEs so no
// duplicate instance), separate TU so the load-time plumbing and
// AMF context stay decoupled — an encode-only or decode-only
// session gets exactly the objects it needs.
//
// Validation status: cross-compile-verified under the Docker
// msvc-wine toolchain; hardware validation pending issue #25.

#if !defined(_WIN32)
#include "decoder.h"
namespace unio {
std::unique_ptr<Decoder> MakeAmfDecoder() { return nullptr; }
}  // namespace unio
#else

#include "decoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include <core/Factory.h>
#include <core/Context.h>
#include <core/Surface.h>
#include <core/Buffer.h>
#include <core/Data.h>
#include <core/Result.h>
#include <components/VideoDecoderUVD.h>
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

using PFN_AMFInit = AMF_RESULT (AMF_CDECL_CALL*)(
    amf_uint64 version, amf::AMFFactory** ppFactory);

class AmfDecoder final : public Decoder {
public:
    AmfDecoder() = default;
    ~AmfDecoder() override { Teardown(); }

    std::optional<std::string> Init(const Config& cfg,
                                    FrameReady on_frame) override {
        cfg_ = cfg;
        on_frame_ = std::move(on_frame);
        if (auto err = LoadDll(); err) return err;

        // Pick the AMD adapter explicitly — mirror of the encoder
        // path. On hybrid boxes the default HARDWARE driver-type
        // lands on whichever adapter Windows prefers, which on
        // typical dual-GPU laptops is the Intel iGPU.
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
            if (d.VendorId == 0x1002) { chosen = a; break; }
        }
        if (!chosen) {
            return "no AMD GPU found (vendor 0x1002)";
        }

        // VIDEO_SUPPORT: same flag decoder_d3d11va uses so the
        // D3D11 device exposes the video-facet interfaces AMF's
        // UVD component needs.
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL wanted[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got = {};
        if (FAILED(D3D11CreateDevice(
                chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                flags, wanted, static_cast<UINT>(std::size(wanted)),
                D3D11_SDK_VERSION, &device_, &got, &ctx_))) {
            return "D3D11CreateDevice on AMD adapter failed";
        }
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
                                        AMFVideoDecoderUVD_H264_AVC,
                                        &decoder_);
        if (ar != AMF_OK || !decoder_) {
            return "CreateComponent(UVD_H264_AVC) failed (rc="
                   + std::to_string(ar) + ")";
        }

        // Reorder mode LOW_LATENCY: AMF's UVD decoder can buffer
        // an entire GOP for B-frame reordering; we never emit B
        // frames (encoder_amf/nvenc/onevpl all disable them), so
        // no reorder is needed and LOW_LATENCY shaves off the
        // input-to-output buffering depth.
        (void)decoder_->SetProperty(
            AMF_VIDEO_DECODER_REORDER_MODE,
            AMF_VIDEO_DECODER_MODE_LOW_LATENCY);
        return std::nullopt;
    }

    std::optional<std::string> Feed(const std::uint8_t* bytes,
                                    std::size_t len) override {
        if (len == 0) return std::nullopt;

        // Parse for SEI / SPS before handing the bitstream to AMF.
        // AMF's decoder ingests full Annex-B packets; it parses
        // SPS / PPS internally, so we only need to catch the
        // latency-SEI ourselves (AMF doesn't expose user SEI on
        // the output side) and lazy-init the decoder on the first
        // IDR we see.
        auto nals = ScanAnnexB(bytes, len);
        bool saw_idr = false;
        for (const auto& n : nals) {
            if (n.length == 0) continue;
            const std::uint8_t* nal = bytes + n.offset;
            const std::uint8_t type = nal[0] & 0x1F;
            const std::uint8_t* body = nal + 1;
            std::size_t body_len = n.length - 1;
            if (type == kNalSps) {
                auto rbsp = StripEmulationPrevention(body, body_len);
                ParsedSps s{};
                if (ParseSps(rbsp.data(), rbsp.size(), s)) {
                    sps_ = s;
                    have_sps_ = true;
                    width_  = sps_.pic_width_in_mbs * 16
                              - 2 * sps_.crop_right_offset;
                    height_ = sps_.pic_height_in_mbs * 16
                              - 2 * sps_.crop_bottom_offset;
                }
            } else if (type == kNalSei) {
                auto rbsp = StripEmulationPrevention(body, body_len);
                std::uint64_t fid = 0, cap_ns = 0;
                if (ParseLatencySei(rbsp.data(), rbsp.size(),
                                    fid, cap_ns)) {
                    pending_frame_id_ = fid;
                    pending_capture_ns_ = cap_ns;
                }
            } else if (type == kNalIdrSlice) {
                saw_idr = true;
            }
        }

        if (!initialized_) {
            if (!have_sps_ || !saw_idr) {
                // Need both the SPS (for dimensions) and an IDR
                // (AMF refuses to start on a non-IDR) before the
                // first Init(). Drop non-IDR packets silently
                // until the first IDR arrives — matches how the
                // d3d11va + onevpl sides handle the pre-IDR gap.
                return std::nullopt;
            }
            AMF_RESULT ar = decoder_->Init(
                amf::AMF_SURFACE_NV12, width_, height_);
            if (ar != AMF_OK) {
                return "AMFDecoder::Init failed (rc="
                       + std::to_string(ar) + ")";
            }
            initialized_ = true;
            std::fprintf(stderr,
                "unio-pipe: AMF decoder context %dx%d NV12\n",
                width_, height_);
        }

        // Wrap the Annex-B packet in an AMFBuffer. AllocBuffer on
        // MEMORY_HOST gives us a CPU-accessible buffer we memcpy
        // into; AMF handles the host-to-device transfer during
        // SubmitInput.
        amf::AMFBufferPtr in_buf;
        AMF_RESULT ar = context_->AllocBuffer(
            amf::AMF_MEMORY_HOST, len, &in_buf);
        if (ar != AMF_OK || !in_buf) {
            return "AllocBuffer failed";
        }
        std::memcpy(in_buf->GetNative(), bytes, len);
        in_buf->SetPts(
            static_cast<amf_pts>(pending_capture_ns_ / 100));
        (void)in_buf->SetProperty(L"unio_capture_ns",
            static_cast<amf_int64>(pending_capture_ns_));
        (void)in_buf->SetProperty(L"unio_frame_id",
            static_cast<amf_int64>(pending_frame_id_));

        ar = decoder_->SubmitInput(in_buf);
        if (ar == AMF_INPUT_FULL) {
            // Decoder queue is full — drain whatever's ready,
            // then retry. Matches the encoder's symmetric path.
            DrainAvailable();
            ar = decoder_->SubmitInput(in_buf);
        }
        if (ar != AMF_OK && ar != AMF_NEED_MORE_INPUT
                         && ar != AMF_REPEAT) {
            return "AMFDecoder::SubmitInput rc="
                   + std::to_string(ar);
        }

        // Drain everything that's ready after this submit. UVD's
        // pipeline depth is typically 1-2 frames, so we might get
        // back exactly the frame we just submitted or one that was
        // queued earlier.
        DrainAvailable();
        return std::nullopt;
    }

    std::string_view Name() const override { return "amf"; }

private:
    void DrainAvailable() {
        while (true) {
            amf::AMFDataPtr out;
            AMF_RESULT ar = decoder_->QueryOutput(&out);
            if (ar == AMF_REPEAT || ar == AMF_NEED_MORE_INPUT
                                 || !out) {
                break;
            }
            if (ar != AMF_OK) {
                std::fprintf(stderr,
                    "unio-pipe: AMF decoder QueryOutput rc=%d\n",
                    static_cast<int>(ar));
                break;
            }
            amf::AMFSurfacePtr surf(out);
            if (!surf) continue;

            // Ensure the surface lives on the D3D11 device we
            // share with the presenter. AMF can decode to system
            // memory or DX11 — we requested DX11 via InitDX11, so
            // a Convert to MEMORY_DX11 is a no-op if already
            // there and a fast device-local copy otherwise.
            ar = surf->Convert(amf::AMF_MEMORY_DX11);
            if (ar != AMF_OK) {
                std::fprintf(stderr,
                    "unio-pipe: AMF surface Convert(DX11) rc=%d\n",
                    static_cast<int>(ar));
                continue;
            }

            amf::AMFPlane* plane = surf->GetPlaneAt(0);
            if (!plane) continue;
            auto* tex = static_cast<ID3D11Texture2D*>(
                plane->GetNative());
            if (!tex) continue;

            // Recover the per-frame capture timestamp we stashed
            // on the input buffer. AMF propagates user properties
            // onto the output surface the same way the encoder's
            // output data carries them.
            amf_int64 cap_ns = 0;
            amf_int64 fid = 0;
            (void)surf->GetProperty(L"unio_capture_ns", &cap_ns);
            (void)surf->GetProperty(L"unio_frame_id", &fid);

            if (on_frame_) {
                DecodedFrame df;
                df.surface_handle =
                    reinterpret_cast<std::uintptr_t>(tex);
                df.native_device =
                    reinterpret_cast<std::uintptr_t>(device_.Get());
                df.width = static_cast<std::uint32_t>(width_);
                df.height = static_cast<std::uint32_t>(height_);
                df.decode_done_monotonic_ns = NowNs();
                df.frame_id = static_cast<std::uint64_t>(fid);
                df.capture_monotonic_ns =
                    static_cast<std::uint64_t>(cap_ns);
                df.key_frame = false;  // AMF doesn't surface IDR flag on output
                // Hold a reference to the AMFSurface until the
                // presenter's frame callback returns — the
                // AMFSurfacePtr's refcount keeps the D3D11
                // texture alive for the duration of Present().
                last_surf_ = surf;
                on_frame_(df);
                pending_frame_id_ = 0;
                pending_capture_ns_ = 0;
            }
        }
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
        last_surf_ = nullptr;
        if (decoder_) {
            decoder_->Terminate();
            decoder_ = nullptr;
        }
        if (context_) {
            context_->Terminate();
            context_ = nullptr;
        }
        factory_ = nullptr;
        ctx_.Reset();
        device_.Reset();
        // Intentionally do NOT FreeLibrary(dll_) — same
        // AMF-worker-thread outlives-Terminate reason the encoder
        // TU documents.
        initialized_ = false;
    }

    Config cfg_{};
    FrameReady on_frame_;

    HMODULE dll_ = nullptr;
    PFN_AMFInit amf_init_ = nullptr;
    amf::AMFFactory* factory_ = nullptr;
    amf::AMFContextPtr context_;
    amf::AMFComponentPtr decoder_;
    amf::AMFSurfacePtr last_surf_;   // keeps the last texture alive across on_frame_
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;

    ParsedSps sps_{};
    bool have_sps_ = false;
    bool initialized_ = false;
    int width_ = 0;
    int height_ = 0;

    std::uint64_t pending_frame_id_ = 0;
    std::uint64_t pending_capture_ns_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> MakeAmfDecoder() {
    return std::make_unique<AmfDecoder>();
}

}  // namespace unio

#endif  // _WIN32
