// Intel oneVPL H.264 decoder (Windows). Sibling of
// decoder_d3d11va.cpp. Annex-B packets arrive via Feed(); the same
// libvpl.dll / libmfxhw64.dll runtime used by encoder_onevpl.cpp
// is loaded here (same DLL, separate session, separate loader TU
// so no cross-TU coupling).
//
// Output path:
//   DecodeFrameAsync (system-memory NV12) → SyncOperation →
//   D3D11 STAGING texture Map/copy → CopyResource →
//   USAGE_DEFAULT SHADER_RESOURCE NV12 texture → on_frame_ callback
//   → DXGI flip presenter (same device, direct SRV sampling).
//
// Steps implemented:
//   3a. Session bring-up: same MFXLoad / MFXInit pattern as encoder.
//   3b. DecodeHeader → QueryIOSurf → MFXVideoDECODE_Init (lazy on
//       the first SPS that arrives, i.e. the first IDR packet).
//   3c. Decode loop: DecodeFrameAsync + SyncOperation.
//   3d. SEI latency extraction (ParseLatencySei) + D3D11 NV12
//       upload for the DXGI presenter.

#if !defined(_WIN32)
#include "decoder.h"
namespace unio {
std::unique_ptr<Decoder> MakeOneVplDecoder() { return nullptr; }
}  // namespace unio
#else

#include "decoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <vpl/mfxvideo.h>
#include <vpl/mfxdispatcher.h>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count());
}

// Runtime loader — mirrors the encoder's OneVplLoader but carries
// only the DECODE-side entry points. LoadLibraryW returns the same
// HMODULE (ref-counted by the OS) if the encoder already loaded it,
// so no duplicate DLL instance.
struct OneVplDecLoader {
    HMODULE lib = nullptr;
    const wchar_t* lib_name = L"";
    bool has_vpl_dispatcher = false;
    bool has_legacy_msdk    = false;

    // Session creation (2.x)
    mfxLoader (MFX_CDECL *MFXLoad)(void) = nullptr;
    void      (MFX_CDECL *MFXUnload)(mfxLoader) = nullptr;
    mfxConfig (MFX_CDECL *MFXCreateConfig)(mfxLoader) = nullptr;
    mfxStatus (MFX_CDECL *MFXSetConfigFilterProperty)(
        mfxConfig, const mfxU8*, mfxVariant) = nullptr;
    mfxStatus (MFX_CDECL *MFXCreateSession)(
        mfxLoader, mfxU32, mfxSession*) = nullptr;

    // Session creation (legacy MSDK)
    mfxStatus (MFX_CDECL *MFXInit)(
        mfxIMPL, mfxVersion*, mfxSession*) = nullptr;

    mfxStatus (MFX_CDECL *MFXClose)(mfxSession) = nullptr;

    // Decode pipeline
    mfxStatus (MFX_CDECL *MFXVideoDECODE_DecodeHeader)(
        mfxSession, mfxBitstream*, mfxVideoParam*) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoDECODE_QueryIOSurf)(
        mfxSession, mfxVideoParam*, mfxFrameAllocRequest*) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoDECODE_Init)(
        mfxSession, mfxVideoParam*) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoDECODE_Close)(mfxSession) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoDECODE_DecodeFrameAsync)(
        mfxSession, mfxBitstream*, mfxFrameSurface1*,
        mfxFrameSurface1**, mfxSyncPoint*) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoCORE_SyncOperation)(
        mfxSession, mfxSyncPoint, mfxU32) = nullptr;

    bool ready = false;
    std::string reason;
};

OneVplDecLoader& DecLoader() {
    static OneVplDecLoader L;
    static std::once_flag flag;
    std::call_once(flag, []() {
        const wchar_t* cands[] = {
            L"libvpl.dll", L"libmfx.dll", L"libmfxhw64.dll"};
        for (auto* name : cands) {
            L.lib = LoadLibraryW(name);
            if (L.lib) { L.lib_name = name; break; }
        }
        if (!L.lib) {
            L.reason = "libvpl.dll / libmfx.dll / libmfxhw64.dll "
                       "not loadable (no Intel graphics driver?)";
            return;
        }
        auto sym = [&](const char* n) -> FARPROC {
            return GetProcAddress(L.lib, n);
        };
        #define LD(n) L.n = reinterpret_cast<decltype(L.n)>(sym(#n))
        LD(MFXLoad); LD(MFXUnload); LD(MFXCreateConfig);
        LD(MFXSetConfigFilterProperty); LD(MFXCreateSession);
        LD(MFXInit); LD(MFXClose);
        LD(MFXVideoDECODE_DecodeHeader);
        LD(MFXVideoDECODE_QueryIOSurf);
        LD(MFXVideoDECODE_Init);
        LD(MFXVideoDECODE_Close);
        LD(MFXVideoDECODE_DecodeFrameAsync);
        LD(MFXVideoCORE_SyncOperation);
        #undef LD
        L.has_vpl_dispatcher = (L.MFXLoad && L.MFXCreateConfig
            && L.MFXSetConfigFilterProperty && L.MFXCreateSession);
        L.has_legacy_msdk = (L.MFXInit != nullptr);
        if (!L.has_vpl_dispatcher && !L.has_legacy_msdk) {
            L.reason = "neither MFXLoad nor MFXInit present";
            return;
        }
        if (!L.MFXClose || !L.MFXVideoDECODE_DecodeFrameAsync
                        || !L.MFXVideoCORE_SyncOperation) {
            L.reason = "missing required DECODE symbols";
            return;
        }
        L.ready = true;
    });
    return L;
}

mfxStatus SetDecFilter(OneVplDecLoader& L, mfxConfig cfg,
                       const char* name, mfxU32 val) {
    mfxVariant v{};
    v.Version.Version = MFX_VARIANT_VERSION;
    v.Type = MFX_VARIANT_TYPE_U32;
    v.Data.U32 = val;
    return L.MFXSetConfigFilterProperty(cfg,
        reinterpret_cast<const mfxU8*>(name), v);
}

// Pool of D3D11 NV12 output textures handed to the presenter.
// kTexCount-frame ring — by the time we cycle back, the GPU has
// long since retired the SRV draw call.
constexpr int kTexCount = 4;

class OneVplDecoder final : public Decoder {
public:
    OneVplDecoder() = default;
    ~OneVplDecoder() override { Teardown(); }

    std::optional<std::string> Init(const Config& cfg,
                                    FrameReady on_frame) override {
        cfg_ = cfg;
        on_frame_ = std::move(on_frame);

        // 3a: session bring-up
        auto& L = DecLoader();
        if (!L.ready) return L.reason;

        if (L.has_vpl_dispatcher) {
            if (auto e = InitViaVplLoader(L)) return e;
        } else {
            if (auto e = InitViaLegacyMsdk(L)) return e;
        }
        std::fprintf(stderr,
            "unio-pipe: oneVPL decode session open "
            "(runtime=%ls, session=%p)\n",
            L.lib_name, static_cast<void*>(session_));

        // 3d: D3D11 device for staging + texture pool.
        // Create here so the first on_frame_ already carries the
        // right native_device pointer. Textures are (re-)allocated
        // lazily once we know picture dimensions from DecodeHeader.
        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL wanted[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr,
            D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            wanted, static_cast<UINT>(std::size(wanted)),
            D3D11_SDK_VERSION, &device_, &got, &ctx_);
        if (FAILED(hr)) {
            return std::string("D3D11CreateDevice failed for "
                               "oneVPL decoder, HR=")
                   + std::to_string(hr);
        }
        return std::nullopt;
    }

    std::optional<std::string> Feed(
            const std::uint8_t* bytes, std::size_t len) override {
        if (!session_) return std::nullopt;
        auto& L = DecLoader();

        // 3d: extract latency SEI before handing to MSDK.
        auto nals = ScanAnnexB(bytes, len);
        pending_frame_id_ = 0;
        pending_capture_ns_ = 0;
        for (auto& sp : nals) {
            if (sp.length < 1) continue;
            const std::uint8_t hdr = bytes[sp.offset];
            const std::uint8_t nal_type = hdr & 0x1F;
            if (nal_type == kNalSei && sp.length > 1) {
                auto rbsp = StripEmulationPrevention(
                    bytes + sp.offset + 1, sp.length - 1);
                ParseLatencySei(rbsp.data(), rbsp.size(),
                                pending_frame_id_,
                                pending_capture_ns_);
            }
        }

        // Compact pending buffer: memmove unconsumed bytes to front.
        if (bs_.DataOffset > 0 && bs_.DataLength > 0) {
            std::memmove(bs_data_.data(),
                         bs_data_.data() + bs_.DataOffset,
                         bs_.DataLength);
            bs_.DataOffset = 0;
            bs_.Data = bs_data_.data();
        } else if (bs_.DataOffset > 0) {
            bs_.DataOffset = 0;
            bs_.DataLength = 0;
        }

        // Append new bytes. Keep MaxLength in sync with the
        // allocated buffer capacity — MSDK returns
        // MFX_ERR_UNDEFINED_BEHAVIOR (-16) if MaxLength == 0.
        const std::size_t tail = static_cast<std::size_t>(
            bs_.DataOffset) + bs_.DataLength;
        if (tail + len > bs_data_.size())
            bs_data_.resize(tail + len + 4096);
        std::memcpy(bs_data_.data() + tail, bytes, len);
        bs_.Data       = bs_data_.data();
        bs_.MaxLength  = static_cast<mfxU32>(bs_data_.size());
        bs_.DataLength = static_cast<mfxU32>(bs_.DataLength + len);
        bs_.DataFlag   = MFX_BITSTREAM_COMPLETE_FRAME;

        // 3b: lazy init — parse the first SPS ourselves (bypassing
        // MFXVideoDECODE_DecodeHeader which returns
        // MFX_ERR_UNDEFINED_BEHAVIOR on the 1.x MSDK compat shim
        // before the session is committed to decode).
        if (!decode_init_done_) {
            for (auto& sp : nals) {
                if (sp.length < 1) continue;
                const std::uint8_t nt =
                    bytes[sp.offset] & 0x1F;
                if (nt != kNalSps) continue;
                auto rbsp = StripEmulationPrevention(
                    bytes + sp.offset + 1, sp.length - 1);
                ParsedSps sps{};
                if (!ParseSps(rbsp.data(), rbsp.size(), sps))
                    continue;
                if (auto e = InitDecodeFromSps(L, sps))
                    return e;
                break;
            }
            if (!decode_init_done_) return std::nullopt;
        }

        // 3c: decode loop — one frame per Feed() in normal flow;
        // loop handles the rare MFX_ERR_MORE_SURFACE case.
        for (int attempts = 0; attempts < 16; ++attempts) {
            mfxFrameSurface1* work = GetFreeSurface();
            if (!work) {
                std::fprintf(stderr,
                    "unio-pipe: oneVPL all decode surfaces "
                    "locked\n");
                return std::nullopt;
            }

            mfxFrameSurface1* out = nullptr;
            mfxSyncPoint sync    = nullptr;
            mfxStatus ds = L.MFXVideoDECODE_DecodeFrameAsync(
                session_, &bs_, work, &out, &sync);

            if (ds == MFX_ERR_MORE_DATA) return std::nullopt;
            if (ds == MFX_ERR_MORE_SURFACE) continue;
            if (ds == MFX_WRN_DEVICE_BUSY) {
                ::Sleep(1);
                continue;
            }
            if (ds < 0) {
                std::fprintf(stderr,
                    "unio-pipe: oneVPL DecodeFrameAsync: %d\n",
                    static_cast<int>(ds));
                return std::nullopt;
            }

            // Wait for GPU decode to finish.
            if (sync) {
                mfxStatus ss = L.MFXVideoCORE_SyncOperation(
                    session_, sync, 5000);
                if (ss < 0) {
                    std::fprintf(stderr,
                        "unio-pipe: oneVPL SyncOperation: %d\n",
                        static_cast<int>(ss));
                    return std::nullopt;
                }
            }

            if (!out) return std::nullopt;

            // 3d: upload NV12 to D3D11 texture for presenter.
            ID3D11Texture2D* tex = UploadToD3d11(out);
            if (!tex) return std::nullopt;

            DecodedFrame df;
            df.surface_handle          =
                reinterpret_cast<std::uintptr_t>(tex);
            df.native_device           =
                reinterpret_cast<std::uintptr_t>(device_.Get());
            df.width                   = out->Info.CropW
                ? out->Info.CropW : out->Info.Width;
            df.height                  = out->Info.CropH
                ? out->Info.CropH : out->Info.Height;
            df.decode_done_monotonic_ns = NowNs();
            df.frame_id               = pending_frame_id_;
            df.capture_monotonic_ns   = pending_capture_ns_;
            df.key_frame              =
                (out->Data.FrameOrder == 0);

            on_frame_(df);
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::string_view Name() const override { return "onevpl"; }

private:
    std::optional<std::string> InitViaVplLoader(
            OneVplDecLoader& L) {
        loader_ = L.MFXLoad();
        if (!loader_) return "MFXLoad returned null";
        mfxConfig cfg = L.MFXCreateConfig(loader_);
        if (!cfg) return "MFXCreateConfig failed";
        if (auto s = SetDecFilter(L, cfg,
                "mfxImplDescription.Impl",
                MFX_IMPL_TYPE_HARDWARE);
                s != MFX_ERR_NONE) {
            return std::string("SetFilter Impl=HARDWARE: ")
                   + std::to_string(s);
        }
        if (auto s = L.MFXCreateSession(
                loader_, 0, &session_);
                s != MFX_ERR_NONE || !session_) {
            return std::string("MFXCreateSession: ")
                   + std::to_string(s);
        }
        return std::nullopt;
    }

    std::optional<std::string> InitViaLegacyMsdk(
            OneVplDecLoader& L) {
        mfxVersion ver{};
        ver.Major = 1; ver.Minor = 35;
        struct Try { const char* name; mfxIMPL impl; };
        const Try tries[] = {
            {"HARDWARE_ANY", MFX_IMPL_HARDWARE_ANY},
            {"HARDWARE",     MFX_IMPL_HARDWARE},
            {"AUTO_ANY",     MFX_IMPL_AUTO_ANY},
            {"AUTO",         MFX_IMPL_AUTO},
        };
        std::string last;
        for (auto& t : tries) {
            mfxVersion v = ver;
            if (L.MFXInit(t.impl, &v, &session_) == MFX_ERR_NONE
                    && session_) {
                return std::nullopt;
            }
            last = t.name;
            session_ = nullptr;
        }
        return "MSDK MFXInit failed (last impl=" + last + ")";
    }

    // Called once on the first SPS. Builds mfxVideoParam from
    // the self-parsed SPS (bypasses DecodeHeader which is
    // unreliable on the 1.x MSDK compat shim before Init).
    std::optional<std::string> InitDecodeFromSps(
            OneVplDecLoader& L, const ParsedSps& sps) {
        std::memset(&vpar_, 0, sizeof(vpar_));
        vpar_.mfx.CodecId        = MFX_CODEC_AVC;
        vpar_.IOPattern          = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
        // Codec-aligned dimensions (multiple of 16 for the MB grid).
        vpar_.mfx.FrameInfo.Width  =
            static_cast<mfxU16>(sps.pic_width_in_mbs * 16);
        vpar_.mfx.FrameInfo.Height =
            static_cast<mfxU16>(sps.pic_height_in_mbs * 16
                * (sps.frame_mbs_only_flag ? 1 : 2));
        // Cropped display dimensions.
        vpar_.mfx.FrameInfo.CropW  =
            static_cast<mfxU16>(vpar_.mfx.FrameInfo.Width
                - sps.crop_right_offset * 2);
        vpar_.mfx.FrameInfo.CropH  =
            static_cast<mfxU16>(vpar_.mfx.FrameInfo.Height
                - sps.crop_bottom_offset * 2);
        vpar_.mfx.FrameInfo.FourCC        = MFX_FOURCC_NV12;
        vpar_.mfx.FrameInfo.ChromaFormat  =
            MFX_CHROMAFORMAT_YUV420;
        vpar_.mfx.FrameInfo.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
        vpar_.mfx.FrameInfo.FrameRateExtN = 30;
        vpar_.mfx.FrameInfo.FrameRateExtD = 1;
        vpar_.mfx.NumRefFrame             =
            static_cast<mfxU16>(sps.num_ref_frames);
        // Profile and level so the driver can choose the right
        // decode path. Map SPS profile_idc to MSDK codec profile.
        switch (sps.profile_idc) {
            case 66:
                vpar_.mfx.CodecProfile =
                    MFX_PROFILE_AVC_CONSTRAINED_BASELINE; break;
            case 77:
                vpar_.mfx.CodecProfile = MFX_PROFILE_AVC_MAIN; break;
            default:
                vpar_.mfx.CodecProfile = MFX_PROFILE_AVC_HIGH; break;
        }
        vpar_.mfx.CodecLevel = static_cast<mfxU16>(sps.level_idc);

        mfxFrameAllocRequest req{};
        if (auto s = L.MFXVideoDECODE_QueryIOSurf(
                session_, &vpar_, &req); s < 0) {
            std::fprintf(stderr,
                "unio-pipe: oneVPL DecodeQueryIOSurf: %d\n",
                static_cast<int>(s));
            // Fall back to a safe default count.
            req.NumFrameSuggested = 8;
        }
        int n = std::max(
            static_cast<int>(req.NumFrameSuggested), 8);

        if (auto e = AllocSurfaces(n)) return e;

        mfxStatus is = L.MFXVideoDECODE_Init(session_, &vpar_);
        if (is < 0) {
            return std::string("MFXVideoDECODE_Init: ")
                   + std::to_string(is);
        }

        const int w = static_cast<int>(vpar_.mfx.FrameInfo.Width);
        const int h = static_cast<int>(vpar_.mfx.FrameInfo.Height);
        std::fprintf(stderr,
            "unio-pipe: oneVPL decoder init OK "
            "(%dx%d, %d surfaces)\n", w, h, n);

        if (auto e = AllocD3dTextures(w, h)) return e;
        decode_init_done_ = true;
        return std::nullopt;
    }

    std::optional<std::string> AllocSurfaces(int n) {
        const int w =
            static_cast<int>(vpar_.mfx.FrameInfo.Width);
        const int h =
            static_cast<int>(vpar_.mfx.FrameInfo.Height);
        const int pitch = (w + 31) & ~31;
        const int luma  = pitch * h;
        const int chroma = pitch * (h / 2);
        const int per   = luma + chroma;

        surface_buf_.assign(
            static_cast<std::size_t>(n * per), 0);
        surfaces_.resize(static_cast<std::size_t>(n));

        for (int i = 0; i < n; ++i) {
            auto& s = surfaces_[i];
            std::memset(&s, 0, sizeof(s));
            s.Info       = vpar_.mfx.FrameInfo;
            s.Data.Y     = surface_buf_.data() +
                           static_cast<std::size_t>(i * per);
            s.Data.UV    = s.Data.Y + luma;
            s.Data.V     = nullptr;
            s.Data.Pitch = static_cast<mfxU16>(pitch);
        }
        return std::nullopt;
    }

    // Allocate kTexCount DXGI_FORMAT_NV12 D3D11 textures.
    // Each is USAGE_DEFAULT + BIND_SHADER_RESOURCE so the DXGI
    // presenter can create R8 / R8G8 SRVs on Y and UV planes.
    std::optional<std::string> AllocD3dTextures(int w, int h) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width              = static_cast<UINT>(w);
        td.Height             = static_cast<UINT>(h);
        td.MipLevels          = 1;
        td.ArraySize          = 1;
        td.Format             = DXGI_FORMAT_NV12;
        td.SampleDesc.Count   = 1;
        td.Usage              = D3D11_USAGE_DEFAULT;
        td.BindFlags          =
            D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;

        for (int i = 0; i < kTexCount; ++i) {
            HRESULT hr = device_->CreateTexture2D(
                &td, nullptr, &out_tex_[i]);
            if (FAILED(hr)) {
                return std::string(
                    "CreateTexture2D (output NV12) failed, HR=")
                    + std::to_string(hr);
            }
        }

        // Staging texture for CPU → GPU upload.
        td.Usage     = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device_->CreateTexture2D(
            &td, nullptr, &staging_tex_);
        if (FAILED(hr)) {
            return std::string(
                "CreateTexture2D (staging NV12) failed, HR=")
                + std::to_string(hr);
        }

        tex_w_ = w; tex_h_ = h;
        return std::nullopt;
    }

    mfxFrameSurface1* GetFreeSurface() {
        for (auto& s : surfaces_)
            if (s.Data.Locked == 0) return &s;
        return nullptr;
    }

    // Copy decoded NV12 from a system-memory MSDK surface into the
    // next D3D11 output texture via the staging texture.
    ID3D11Texture2D* UploadToD3d11(mfxFrameSurface1* surf) {
        if (!surf || !staging_tex_) return nullptr;

        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx_->Map(
            staging_tex_.Get(), 0,
            D3D11_MAP_WRITE, 0, &m);
        if (FAILED(hr)) {
            std::fprintf(stderr,
                "unio-pipe: oneVPL dec staging Map failed "
                "HR=%08lx\n",
                static_cast<unsigned long>(hr));
            return nullptr;
        }

        const int w     = tex_w_;
        const int h     = tex_h_;
        const int spitch = static_cast<int>(surf->Data.Pitch);
        const int dpitch = static_cast<int>(m.RowPitch);
        auto* dst = static_cast<std::uint8_t*>(m.pData);

        // Y plane: h rows.
        for (int r = 0; r < h; ++r) {
            std::memcpy(dst + r * dpitch,
                        surf->Data.Y  + r * spitch,
                        static_cast<std::size_t>(w));
        }
        // UV plane: h/2 rows, immediately after Y in the NV12
        // staging layout.
        auto* dst_uv = dst + dpitch * h;
        for (int r = 0; r < h / 2; ++r) {
            std::memcpy(dst_uv + r * dpitch,
                        surf->Data.UV + r * spitch,
                        static_cast<std::size_t>(w));
        }
        ctx_->Unmap(staging_tex_.Get(), 0);

        // Advance ring index and copy staging → output texture.
        int idx = tex_idx_;
        tex_idx_ = (tex_idx_ + 1) % kTexCount;
        ctx_->CopyResource(
            out_tex_[idx].Get(), staging_tex_.Get());

        return out_tex_[idx].Get();
    }

    void Teardown() {
        auto& L = DecLoader();
        if (session_) {
            if (L.MFXVideoDECODE_Close)
                L.MFXVideoDECODE_Close(session_);
            L.MFXClose(session_);
            session_ = nullptr;
        }
        if (loader_ && L.MFXUnload) {
            L.MFXUnload(loader_);
            loader_ = nullptr;
        }
    }

    Config     cfg_{};
    FrameReady on_frame_;
    mfxLoader  loader_  = nullptr;
    mfxSession session_ = nullptr;
    mfxVideoParam vpar_{};
    bool decode_init_done_ = false;

    // SEI latency state — last extracted values before this feed.
    std::uint64_t pending_frame_id_    = 0;
    std::uint64_t pending_capture_ns_  = 0;

    // System-memory NV12 surface pool (decoder output).
    std::vector<mfxFrameSurface1> surfaces_;
    std::vector<std::uint8_t>     surface_buf_;

    // Compacting bitstream buffer fed to DecodeFrameAsync.
    std::vector<std::uint8_t> bs_data_;
    mfxBitstream              bs_{};

    // D3D11 resources for presenter upload.
    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11Texture2D>     out_tex_[kTexCount];
    ComPtr<ID3D11Texture2D>     staging_tex_;
    int tex_idx_ = 0;
    int tex_w_   = 0;
    int tex_h_   = 0;
};

}  // namespace

std::unique_ptr<Decoder> MakeOneVplDecoder() {
    return std::make_unique<OneVplDecoder>();
}

}  // namespace unio
#endif  // _WIN32
