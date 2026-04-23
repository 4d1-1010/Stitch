// Intel oneVPL H.264 encoder (Windows). Sibling of
// encoder_nvenc.cpp. Consumes BGRA CpuFrames from WGC (same shape
// the Windows NVENC path uses) + emits Annex-B H.264 NAL bytes.
//
// Runtime: libvpl.dll (Intel driver 2021+) or libmfx.dll (legacy
// MSDK driver pre-2021). Both accepted — the dispatcher picks up
// whichever is present. LoadLibraryW at startup, GetProcAddress
// the handful of oneVPL entry points we need. Zero build-time
// dep on Intel's SDK (libvpl repo provides headers only; runtime
// ships with the Intel graphics driver).
//
// Build-up strategy (per the step-by-step plan on #26):
//   2a. Session bring-up — MFXLoad → filter chain → MFXCreateSession.
//   2b. Query + QueryIOSurf + MFXVideoENCODE_Init.
//   2c. Real encode loop — surface pool + EncodeFrameAsync + SyncOperation.
//   2d. Per-frame IDR + latency SEI prefix.
//
// All four steps implemented in this file.

#if !defined(_WIN32)
#include "encoder.h"
namespace unio {
std::unique_ptr<Encoder> MakeOneVplEncoder() { return nullptr; }
}  // namespace unio
#else

#include "encoder.h"
#include "h264_parse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

// libvpl 2.x headers (MIT, fetched via CMake FetchContent).
#include <vpl/mfxvideo.h>
#include <vpl/mfxdispatcher.h>

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count());
}

// Runtime loader for the Intel video runtime. Tries three shared
// libraries in order of preference:
//   1. libvpl.dll   — modern oneVPL 2.x dispatcher (Intel Arc,
//                     driver 30.x+, Intel oneAPI Toolkit).
//   2. libmfx.dll   — legacy MSDK dispatcher. Ships on some older
//                     drivers; may or may not expose the 2.x
//                     MFXLoad symbol.
//   3. libmfxhw64.dll — legacy MSDK hardware-specific runtime
//                     (Intel drivers 26.x-28.x on Coffee Lake /
//                     Skylake era). Only exposes the classic
//                     MFXInit API, not the 2.x MFXLoad chain.
//
// Two session-creation codepaths behind the single Init():
//   - If MFXLoad is available: oneVPL 2.x filter chain +
//     MFXCreateSession (preferred).
//   - Else if MFXInit is available: classic MSDK path, impl =
//     HARDWARE, API version 1.35. Gives us a working session on
//     older Intel drivers that can't do the 2.x dispatcher.
//
// Everything else (MFXVideoENCODE_Init, EncodeFrameAsync,
// SyncOperation, Close) takes the same mfxSession regardless of
// which dispatcher created it, so the encode loop in step 2c
// doesn't care.
struct OneVplLoader {
    HMODULE lib = nullptr;
    const wchar_t* lib_name = L"";
    bool has_vpl_dispatcher = false;   // MFXLoad path available
    bool has_legacy_msdk = false;      // MFXInit path available

    // 2.x dispatcher (may be null on legacy runtimes)
    mfxLoader (MFX_CDECL *MFXLoad)(void) = nullptr;
    void (MFX_CDECL *MFXUnload)(mfxLoader) = nullptr;
    mfxConfig (MFX_CDECL *MFXCreateConfig)(mfxLoader) = nullptr;
    mfxStatus (MFX_CDECL *MFXSetConfigFilterProperty)(
        mfxConfig, const mfxU8*, mfxVariant) = nullptr;
    mfxStatus (MFX_CDECL *MFXCreateSession)(
        mfxLoader, mfxU32, mfxSession*) = nullptr;

    // Classic MSDK (may be null on runtimes that only have 2.x)
    mfxStatus (MFX_CDECL *MFXInit)(mfxIMPL, mfxVersion*,
                                    mfxSession*) = nullptr;

    // Shared — all runtimes expose these once we have a session.
    mfxStatus (MFX_CDECL *MFXClose)(mfxSession) = nullptr;

    // Video core + encode (used in later steps).
    mfxStatus (MFX_CDECL *MFXVideoENCODE_Query)(
        mfxSession, mfxVideoParam*, mfxVideoParam*) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoENCODE_QueryIOSurf)(
        mfxSession, mfxVideoParam*, mfxFrameAllocRequest*) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoENCODE_Init)(
        mfxSession, mfxVideoParam*) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoENCODE_Close)(mfxSession) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoENCODE_EncodeFrameAsync)(
        mfxSession, mfxEncodeCtrl*, mfxFrameSurface1*,
        mfxBitstream*, mfxSyncPoint*) = nullptr;
    mfxStatus (MFX_CDECL *MFXVideoCORE_SyncOperation)(
        mfxSession, mfxSyncPoint, mfxU32) = nullptr;
    // MFXMemory_GetSurfaceForEncode is used in step 2c for the
    // internal-alloc surface pool (oneVPL 2.x only; legacy MSDK
    // needs explicit surface allocation).
    mfxStatus (MFX_CDECL *MFXMemory_GetSurfaceForEncode)(
        mfxSession, mfxFrameSurface1**) = nullptr;

    std::string reason;
    bool ready = false;
};

OneVplLoader& Loader() {
    static OneVplLoader L;
    static std::once_flag flag;
    std::call_once(flag, []() {
        struct LibCand { const wchar_t* name; };
        const LibCand cands[] = {
            {L"libvpl.dll"},
            {L"libmfx.dll"},
            {L"libmfxhw64.dll"},
        };
        for (const auto& c : cands) {
            L.lib = LoadLibraryW(c.name);
            if (L.lib) { L.lib_name = c.name; break; }
        }
        if (!L.lib) {
            L.reason = "libvpl.dll / libmfx.dll / libmfxhw64.dll "
                       "not loadable (no Intel graphics driver?)";
            return;
        }

        auto sym = [&](const char* name) -> FARPROC {
            return GetProcAddress(L.lib, name);
        };

        #define LOAD_SYM(n) \
            L.n = reinterpret_cast<decltype(L.n)>(sym(#n))
        LOAD_SYM(MFXLoad);
        LOAD_SYM(MFXUnload);
        LOAD_SYM(MFXCreateConfig);
        LOAD_SYM(MFXSetConfigFilterProperty);
        LOAD_SYM(MFXCreateSession);
        LOAD_SYM(MFXInit);
        LOAD_SYM(MFXClose);
        LOAD_SYM(MFXVideoENCODE_Query);
        LOAD_SYM(MFXVideoENCODE_QueryIOSurf);
        LOAD_SYM(MFXVideoENCODE_Init);
        LOAD_SYM(MFXVideoENCODE_Close);
        LOAD_SYM(MFXVideoENCODE_EncodeFrameAsync);
        LOAD_SYM(MFXVideoCORE_SyncOperation);
        LOAD_SYM(MFXMemory_GetSurfaceForEncode);
        #undef LOAD_SYM

        // Which session-creation path is available?
        L.has_vpl_dispatcher = (L.MFXLoad && L.MFXCreateConfig
                                  && L.MFXSetConfigFilterProperty
                                  && L.MFXCreateSession);
        L.has_legacy_msdk = (L.MFXInit != nullptr);

        if (!L.has_vpl_dispatcher && !L.has_legacy_msdk) {
            L.reason = "neither MFXLoad (oneVPL 2.x) nor MFXInit "
                       "(legacy MSDK) present in the runtime "
                       "— unknown Intel driver variant";
            return;
        }

        // MFXClose is shared between both paths; if it's missing
        // we can't teardown cleanly, which means we shouldn't
        // bring anything up at all.
        if (!L.MFXClose) {
            L.reason = "MFXClose symbol missing";
            return;
        }

        L.ready = true;
    });
    return L;
}

// Set a single config filter property on a created mfxConfig. The
// name is the oneVPL property-path string — e.g.
// "mfxImplDescription.mfxEncoderDescription.encoder.CodecID".
// Returns MFX_ERR_NONE on success, a negative mfxStatus on
// failure.
mfxStatus SetFilter(OneVplLoader& L, mfxConfig cfg,
                     const char* name, mfxU32 u32_val) {
    mfxVariant v{};
    v.Version.Version = MFX_VARIANT_VERSION;
    v.Type = MFX_VARIANT_TYPE_U32;
    v.Data.U32 = u32_val;
    return L.MFXSetConfigFilterProperty(cfg,
        reinterpret_cast<const mfxU8*>(name), v);
}

class OneVplEncoder final : public Encoder {
public:
    OneVplEncoder() = default;
    ~OneVplEncoder() override { Teardown(); }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;
        auto& L = Loader();
        if (!L.ready) return L.reason;

        // Prefer the oneVPL 2.x dispatcher (MFXLoad + filter
        // chain) when present. Falls back to the classic MSDK
        // MFXInit path on older Intel drivers that ship
        // libmfxhw64.dll / libmfx.dll without the 2.x symbols.
        std::string session_path;
        if (L.has_vpl_dispatcher) {
            if (auto err = InitViaVplLoader(L)) return err;
            session_path = "oneVPL 2.x dispatcher";
        } else if (L.has_legacy_msdk) {
            if (auto err = InitViaLegacyMsdk(L)) return err;
            session_path = "legacy MSDK MFXInit";
        } else {
            return "neither oneVPL 2.x nor legacy MSDK dispatcher "
                   "available (loader said ready but neither path "
                   "resolved — internal error)";
        }

        wchar_t lib_short[64];
        std::wcsncpy(lib_short, L.lib_name, 63);
        lib_short[63] = L'\0';
        std::fprintf(stderr,
            "unio-pipe: oneVPL session open via %s "
            "(runtime=%ls) — H.264 HW encoder, session=%p\n",
            session_path.c_str(),
            lib_short,
            static_cast<void*>(session_));

        if (auto err = InitEncode(L)) return err;
        return std::nullopt;
    }

    void ForceIdr() override {
        force_idr_.store(true, std::memory_order_release);
    }

    EncodedPacketPtr Encode(const CpuFrame& frame) override {
        if (!encode_init_done_) return nullptr;
        auto& L = Loader();

        mfxFrameSurface1* surf = nullptr;
        bool using_frame_interface = false;

        if (use_frame_interface_ && L.MFXMemory_GetSurfaceForEncode) {
            // Preferred path (native oneVPL 2.x): GetSurfaceForEncode
            // returns a surface from the encoder's internal pool. Map it
            // for CPU write, fill NV12, Unmap. On 1.x-compat shim sessions
            // this call returns MFX_ERR_INVALID_HANDLE (-6); we detect that
            // and fall through to the manual pool on the first failure.
            mfxStatus gs = L.MFXMemory_GetSurfaceForEncode(session_, &surf);
            if (gs >= 0 && surf) {
                if (surf->FrameInterface && surf->FrameInterface->Map)
                    surf->FrameInterface->Map(surf, MFX_MAP_WRITE);
                BgraToNv12(frame, surf);
                if (surf->FrameInterface && surf->FrameInterface->Unmap)
                    surf->FrameInterface->Unmap(surf);
                using_frame_interface = true;
            } else {
                // 1.x-compat shim: GetSurfaceForEncode unsupported.
                // Disable for all subsequent frames and use manual pool.
                std::fprintf(stderr,
                    "unio-pipe: GetSurfaceForEncode: %d — "
                    "falling back to manual surface pool\n",
                    static_cast<int>(gs));
                use_frame_interface_ = false;
                surf = nullptr;
            }
        }

        if (!surf) {
            // Manual system-memory surface pool (1.x shim or fallback).
            if (surfaces_.empty()) {
                if (auto err = AllocSurfaces()) {
                    std::fprintf(stderr,
                        "unio-pipe: oneVPL surface alloc failed: %s\n",
                        err->c_str());
                    return nullptr;
                }
            }
            for (auto& s : surfaces_) {
                if (s.Data.Locked == 0) { surf = &s; break; }
            }
            if (!surf) {
                std::fprintf(stderr,
                    "unio-pipe: oneVPL all surfaces locked — "
                    "dropping frame %llu\n",
                    static_cast<unsigned long long>(frame.frame_id));
                return nullptr;
            }
            BgraToNv12(frame, surf);
        }
        // Release helper — returns our reference after EncodeFrameAsync.
        // The encoder takes its own ref on submit, so we can release
        // ours immediately after SyncOperation (or on any error path).
        // Only used for the GetSurfaceForEncode path; the manual-pool
        // fallback is managed by Data.Locked, not FrameInterface.
        auto release_surf = [&]() {
            if (using_frame_interface && surf &&
                    surf->FrameInterface && surf->FrameInterface->Release) {
                surf->FrameInterface->Release(surf);
                surf = nullptr;
            }
        };

        mfxEncodeCtrl ctrl{};
        ctrl.FrameType = MFX_FRAMETYPE_UNKNOWN;
        if (force_idr_.exchange(false, std::memory_order_acq_rel)) {
            ctrl.FrameType =
                MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
        }

        surf->Data.TimeStamp =
            static_cast<mfxU64>(frame.capture_monotonic_ns / 100);

        mfxSyncPoint sync_point = nullptr;
        mfxStatus es = L.MFXVideoENCODE_EncodeFrameAsync(
            session_, &ctrl, surf, &bs_, &sync_point);

        if (es == MFX_WRN_DEVICE_BUSY) {
            // Driver resource temporarily unavailable; retry once.
            Sleep(1);
            es = L.MFXVideoENCODE_EncodeFrameAsync(
                session_, &ctrl, surf, &bs_, &sync_point);
        }

        if (es == MFX_ERR_MORE_DATA) {
            // Encoder accepted the frame but hasn't produced output yet
            // (pipeline fill). MFX_ERR_MORE_DATA is negative (-10) so
            // it MUST be checked before the generic es < 0 branch below.
            release_surf();
            return nullptr;
        }
        if (es < 0) {
            std::fprintf(stderr,
                "unio-pipe: EncodeFrameAsync returned %d "
                "(surf: Y=%p UV=%p Pitch=%d Locked=%d "
                "Info.W=%d Info.H=%d)\n",
                static_cast<int>(es),
                static_cast<void*>(surf->Data.Y),
                static_cast<void*>(surf->Data.UV),
                static_cast<int>(surf->Data.Pitch),
                static_cast<int>(surf->Data.Locked),
                static_cast<int>(surf->Info.Width),
                static_cast<int>(surf->Info.Height));
            release_surf();
            return nullptr;
        }

        // Sync: wait up to 100 ms for the encoded bitstream.
        if (sync_point) {
            mfxStatus ss = L.MFXVideoCORE_SyncOperation(
                session_, sync_point, 100);
            if (ss < 0) {
                std::fprintf(stderr,
                    "unio-pipe: SyncOperation returned %d\n",
                    static_cast<int>(ss));
                release_surf();
                return nullptr;
            }
        }

        // Release our surface ref — encoder holds its own until done.
        release_surf();

        if (bs_.DataLength == 0) return nullptr;

        const bool is_key =
            (bs_.FrameType & (MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR)) != 0;
        auto pkt = std::make_unique<EncodedPacket>();
        pkt->frame_id = frame.frame_id;
        pkt->capture_monotonic_ns  = frame.capture_monotonic_ns;
        pkt->encode_done_monotonic_ns = NowNs();
        pkt->key_frame = is_key;

        // Prepend the latency SEI (unregistered user data, same UUID as
        // VA-API / NVENC). The decoder on the receive side parses it to
        // recover frame_id + capture_ns for the latency CSV.
        auto sei = BuildLatencySeiAnnexB(
            pkt->frame_id, pkt->capture_monotonic_ns);
        pkt->nal_bytes.reserve(sei.size() + bs_.DataLength);
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                              sei.begin(), sei.end());
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                              bs_.Data + bs_.DataOffset,
                              bs_.Data + bs_.DataOffset + bs_.DataLength);

        // Reset bitstream position for the next frame.
        bs_.DataOffset = 0;
        bs_.DataLength = 0;

        return pkt;
    }

    std::string_view Name() const override { return "onevpl"; }

private:
    // oneVPL 2.x path: MFXLoad + filter chain + MFXCreateSession.
    std::optional<std::string> InitViaVplLoader(OneVplLoader& L) {
        loader_ = L.MFXLoad();
        if (!loader_) return "MFXLoad returned null";

        // Filter 1: implementation type = hardware. Only *strictly*
        // required filter — we refuse to fall back to the software
        // (x86) path under any circumstances. The dispatcher will
        // load either a 2.x native HW runtime or, on older Intel
        // drivers (UHD 630 / Coffee Lake etc.), the 1.x-compat shim
        // over libmfxhw64.dll; both report IMPL_TYPE_HARDWARE.
        mfxConfig cfg1 = L.MFXCreateConfig(loader_);
        if (!cfg1) return "MFXCreateConfig #1 failed";
        if (auto s = SetFilter(L, cfg1,
                "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
            s != MFX_ERR_NONE) {
            return std::string("SetFilter Impl=HARDWARE failed: ")
                   + std::to_string(s);
        }

        // Deliberately NOT filtered here:
        //   - codec = H.264: the 1.x-compat shim doesn't populate
        //     mfxEncoderDescription at dispatcher-enumeration time,
        //     so adding "encoder.CodecID = AVC" kicks the backend
        //     out of the result set on older drivers. We catch an
        //     AVC-unsupported GPU later via MFXVideoENCODE_Query
        //     (step 2b), where the codec support is actually
        //     reported.
        //   - API version >= 2.0: ditto. Diana's UHD 630 loads
        //     through the 1.x shim which reports API 1.35; strict
        //     2.x filtering rejects it with MFX_ERR_NOT_FOUND (-9).
        //     The API surface we use (MFXVideoENCODE_*) is the
        //     common subset between 1.x and 2.x, so either works.
        //
        // adapter_num=0 picks the first matching implementation —
        // on dual-GPU Intel+NVIDIA hosts that's the Intel iGPU.
        if (auto s = L.MFXCreateSession(loader_, 0, &session_);
            s != MFX_ERR_NONE || !session_) {
            return std::string("MFXCreateSession failed: ")
                   + std::to_string(s);
        }
        return std::nullopt;
    }

    // Legacy MSDK path: MFXInit with classic impl flags. Drivers
    // that ship libmfxhw64.dll on pre-oneVPL-2.x era (Intel
    // graphics 26.x - 28.x, Coffee Lake / Skylake / Broadwell).
    //
    // The classic dispatcher is libmfx.dll, which internally
    // loads libmfxhw64.dll (the hardware backend) when it picks
    // a hardware impl. If the user only has libmfxhw64.dll
    // present (no libmfx.dll / libvpl.dll), we're loading the
    // hw backend directly — its MFXInit expects a specific impl
    // mode. We try the common set in order and return the last
    // error to the caller.
    std::optional<std::string> InitViaLegacyMsdk(OneVplLoader& L) {
        // API 1.35 = last MSDK release.
        mfxVersion ver{};
        ver.Major = 1;
        ver.Minor = 35;
        struct Attempt {
            const char* name;
            mfxIMPL     impl;
        };
        const Attempt attempts[] = {
            // HARDWARE_ANY auto-picks any available HW backend.
            {"MFX_IMPL_HARDWARE_ANY", MFX_IMPL_HARDWARE_ANY},
            // HARDWARE (specific to adapter 0 — typical iGPU).
            {"MFX_IMPL_HARDWARE",     MFX_IMPL_HARDWARE},
            // AUTO lets the dispatcher choose HW first then SW.
            {"MFX_IMPL_AUTO_ANY",     MFX_IMPL_AUTO_ANY},
            {"MFX_IMPL_AUTO",         MFX_IMPL_AUTO},
            // Specific adapter slots — libmfxhw64 sometimes only
            // accepts these when loaded directly.
            {"MFX_IMPL_HARDWARE2",    MFX_IMPL_HARDWARE2},
            {"MFX_IMPL_HARDWARE3",    MFX_IMPL_HARDWARE3},
            {"MFX_IMPL_HARDWARE4",    MFX_IMPL_HARDWARE4},
        };
        std::string last_err = "(no attempt run)";
        for (const auto& a : attempts) {
            mfxVersion v = ver;  // MFXInit may update in-out
            mfxStatus s = L.MFXInit(a.impl, &v, &session_);
            if (s == MFX_ERR_NONE && session_) {
                std::fprintf(stderr,
                    "unio-pipe: MSDK MFXInit succeeded via %s "
                    "(picked API %u.%u)\n",
                    a.name, static_cast<unsigned>(v.Major),
                    static_cast<unsigned>(v.Minor));
                return std::nullopt;
            }
            last_err = std::string("MFXInit(") + a.name + ") = "
                       + std::to_string(s);
            session_ = nullptr;
        }
        return "legacy MSDK MFXInit declined every impl mode "
               "(last: " + last_err + "). libmfxhw64.dll alone "
               "is only the hardware backend; full support "
               "needs libmfx.dll (MSDK dispatcher) or libvpl.dll "
               "(oneVPL 2.x) installed alongside it.";
    }

    // Allocate num_surfaces_needed_ NV12 system-memory surfaces.
    std::optional<std::string> AllocSurfaces() {
        const int n = std::max(num_surfaces_needed_, 8);
        const int w = static_cast<int>(vpar_.mfx.FrameInfo.Width);
        const int h = static_cast<int>(vpar_.mfx.FrameInfo.Height);

        // Allocate one contiguous buffer backing all surfaces so
        // we can keep the surface array stable (pointer-stable
        // requires surfaces_ not to resize after first call — we
        // size it once here).
        const int luma_size   = w * h;
        const int chroma_size = w * h / 2;  // NV12: UV interleaved
        const int per_frame   = luma_size + chroma_size;
        surface_buf_.resize(static_cast<std::size_t>(n * per_frame), 0);
        surfaces_.resize(static_cast<std::size_t>(n));

        // Pitch must be 32-byte aligned per MSDK requirements for
        // system-memory NV12. Width is already 16-aligned from Init,
        // so round up to the next multiple of 32.
        const int pitch = (w + 31) & ~31;

        // Recompute actual backing size with the real pitch.
        const int luma_pitch   = pitch * h;
        const int chroma_pitch = pitch * h / 2;
        const int per_frame_p  = luma_pitch + chroma_pitch;
        surface_buf_.assign(
            static_cast<std::size_t>(n * per_frame_p), 0);

        for (int i = 0; i < n; ++i) {
            auto& s = surfaces_[i];
            std::memset(&s, 0, sizeof(s));
            // Info must exactly match vpar_.mfx.FrameInfo (MFX 1.x
            // requirement). Width stays as the codec-aligned value,
            // NOT the stride. Pitch carries the actual row width.
            s.Info           = vpar_.mfx.FrameInfo;
            s.Data.Y         = surface_buf_.data() +
                               static_cast<std::size_t>(i * per_frame_p);
            s.Data.UV        = s.Data.Y + luma_pitch;
            s.Data.V         = nullptr;  // UV interleaved in NV12
            s.Data.Pitch     = static_cast<mfxU16>(pitch);
            // MemType: leave as 0 for user-managed system memory.
        }

        return std::nullopt;
    }

    // Convert BGRA32 (BGRX) → NV12 into *surf.
    // BT.601 limited-range coefficients — same as WGC path used by
    // the NVENC encoder on Windows.
    static void BgraToNv12(const CpuFrame& f, mfxFrameSurface1* surf) {
        const int sw    = static_cast<int>(f.width);
        const int sh    = static_cast<int>(f.height);
        const int pitch = static_cast<int>(surf->Data.Pitch);
        mfxU8* Y  = surf->Data.Y;
        mfxU8* UV = surf->Data.UV;

        for (int row = 0; row < sh; ++row) {
            const std::uint8_t* src =
                f.pixels.data() +
                static_cast<std::size_t>(row) * f.stride_bytes;
            mfxU8* dst_y = Y + static_cast<std::size_t>(row) * pitch;
            for (int col = 0; col < sw; ++col) {
                int b = src[col * 4 + 0];
                int g = src[col * 4 + 1];
                int r = src[col * 4 + 2];
                // BT.601 limited-range: Y=[16,235]
                int y = ((66*r + 129*g + 25*b + 128) >> 8) + 16;
                dst_y[col] = static_cast<mfxU8>(y);
            }
        }

        // Chroma: one UV pair per 2×2 block (subsample from even rows).
        const int chroma_rows = sh / 2;
        for (int row = 0; row < chroma_rows; ++row) {
            const std::uint8_t* src0 =
                f.pixels.data() +
                static_cast<std::size_t>(row * 2) * f.stride_bytes;
            mfxU8* dst_uv = UV + static_cast<std::size_t>(row) * pitch;
            for (int col = 0; col < sw / 2; ++col) {
                // Average the two horizontal pixels.
                int b = ((int)src0[col*8+0] + (int)src0[col*8+4]) >> 1;
                int g = ((int)src0[col*8+1] + (int)src0[col*8+5]) >> 1;
                int r = ((int)src0[col*8+2] + (int)src0[col*8+6]) >> 1;
                // BT.601 limited Cb=[16,240], Cr=[16,240]
                int cb = ((-38*r - 74*g + 112*b + 128) >> 8) + 128;
                int cr = ((112*r - 94*g - 18*b + 128) >> 8) + 128;
                dst_uv[col * 2 + 0] = static_cast<mfxU8>(cb);
                dst_uv[col * 2 + 1] = static_cast<mfxU8>(cr);
            }
        }
    }

    // Step 2b: fill mfxVideoParam, Query capability, QueryIOSurf,
    // then call MFXVideoENCODE_Init. Returns an error string on
    // failure; nullopt on success.
    std::optional<std::string> InitEncode(OneVplLoader& L) {
        if (!L.MFXVideoENCODE_Query || !L.MFXVideoENCODE_QueryIOSurf
            || !L.MFXVideoENCODE_Init) {
            return "MFXVideoENCODE_Query/QueryIOSurf/Init "
                   "symbol missing — driver too old?";
        }

        std::memset(&vpar_, 0, sizeof(vpar_));
        vpar_.mfx.CodecId             = MFX_CODEC_AVC;
        vpar_.mfx.CodecProfile        = MFX_PROFILE_AVC_MAIN;
        vpar_.mfx.TargetUsage         = MFX_TARGETUSAGE_BALANCED;
        // CQP mode — quality==20 means QP=20 for all frame types.
        vpar_.mfx.RateControlMethod   = MFX_RATECONTROL_CQP;
        vpar_.mfx.QPI = vpar_.mfx.QPP = vpar_.mfx.QPB =
            static_cast<mfxU16>(cfg_.quality);
        vpar_.mfx.GopPicSize          = 0;    // IDR-only on keyframe
        vpar_.mfx.GopRefDist          = 1;    // no B-frames
        vpar_.mfx.IdrInterval         = 0;    // first frame = IDR
        vpar_.mfx.EncodedOrder        = 0;    // display order
        vpar_.mfx.FrameInfo.FourCC    = MFX_FOURCC_NV12;
        vpar_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
        vpar_.mfx.FrameInfo.Width     =
            static_cast<mfxU16>((cfg_.width  + 15) & ~15);  // 16-align
        vpar_.mfx.FrameInfo.Height    =
            static_cast<mfxU16>((cfg_.height + 15) & ~15);
        vpar_.mfx.FrameInfo.CropX     = 0;
        vpar_.mfx.FrameInfo.CropY     = 0;
        vpar_.mfx.FrameInfo.CropW     = static_cast<mfxU16>(cfg_.width);
        vpar_.mfx.FrameInfo.CropH     = static_cast<mfxU16>(cfg_.height);
        vpar_.mfx.FrameInfo.FrameRateExtN =
            static_cast<mfxU32>(cfg_.fps);
        vpar_.mfx.FrameInfo.FrameRateExtD = 1;
        vpar_.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
        // Internal surface allocation (oneVPL 2.x / MSDK 1.x both
        // support this for software-managed upload). The host CPU
        // copies NV12 into the surface pointer returned by
        // MFXMemory_GetSurfaceForEncode or mfxFrameSurface1::Data.
        vpar_.IOPattern = MFX_IOPATTERN_IN_SYSTEM_MEMORY;

        // Query lets the driver correct any field it doesn't support
        // (e.g. unsupported QP range, unsupported width alignment).
        // We treat a negative return as failure; MFX_WRN_PARTIAL_ACCELERATION
        // (4) and MFX_WRN_INCOMPATIBLE_VIDEO_PARAM (3) are warnings
        // the driver is telling us it adjusted something — not fatal.
        mfxVideoParam q = vpar_;
        mfxStatus qs = L.MFXVideoENCODE_Query(session_, &q, &q);
        if (qs < 0) {
            return std::string("MFXVideoENCODE_Query failed: ")
                   + std::to_string(qs);
        }
        if (qs != MFX_ERR_NONE) {
            std::fprintf(stderr,
                "unio-pipe: MFXVideoENCODE_Query warning %d "
                "(driver adjusted params; continuing)\n",
                static_cast<int>(qs));
        }
        vpar_ = q;

        // QueryIOSurf gives us the required surface counts for the
        // system-memory pool. The returned mfxFrameAllocRequest
        // NumFrameSuggested / NumFrameMin is advisory — we allocate
        // surfaces lazily in Encode() per step 2c.
        mfxFrameAllocRequest req{};
        mfxStatus ioqs = L.MFXVideoENCODE_QueryIOSurf(session_,
                                                        &vpar_, &req);
        if (ioqs < 0) {
            return std::string("MFXVideoENCODE_QueryIOSurf failed: ")
                   + std::to_string(ioqs);
        }
        num_surfaces_needed_ = static_cast<int>(req.NumFrameSuggested);
        std::fprintf(stderr,
            "unio-pipe: oneVPL QueryIOSurf: NumFrameSuggested=%d "
            "NumFrameMin=%d\n",
            static_cast<int>(req.NumFrameSuggested),
            static_cast<int>(req.NumFrameMin));

        mfxStatus is = L.MFXVideoENCODE_Init(session_, &vpar_);
        if (is < 0) {
            return std::string("MFXVideoENCODE_Init failed: ")
                   + std::to_string(is);
        }
        if (is != MFX_ERR_NONE) {
            std::fprintf(stderr,
                "unio-pipe: MFXVideoENCODE_Init warning %d\n",
                static_cast<int>(is));
        }
        // Bitstream output buffer — allocated here so it's ready
        // regardless of whether Encode() takes the GetSurfaceForEncode
        // path (2.x) or the manual surface-pool fallback.
        const int bsw = static_cast<int>(vpar_.mfx.FrameInfo.Width);
        const int bsh = static_cast<int>(vpar_.mfx.FrameInfo.Height);
        bs_buf_.resize(static_cast<std::size_t>(bsw * bsh * 4), 0);
        std::memset(&bs_, 0, sizeof(bs_));
        bs_.Data      = bs_buf_.data();
        bs_.MaxLength = static_cast<mfxU32>(bs_buf_.size());

        encode_init_done_ = true;
        std::fprintf(stderr,
            "unio-pipe: oneVPL MFXVideoENCODE_Init OK — "
            "%dx%d CQP QP=%d fps=%d\n",
            cfg_.width, cfg_.height, cfg_.quality, cfg_.fps);
        return std::nullopt;
    }

    void Teardown() {
        auto& L = Loader();
        if (!L.ready) return;
        // MFXVideoENCODE_Close is a no-op when the encoder wasn't
        // initialised — safe to call.
        if (session_ && L.MFXVideoENCODE_Close) {
            L.MFXVideoENCODE_Close(session_);
        }
        if (session_) {
            L.MFXClose(session_);
            session_ = nullptr;
        }
        // Only the 2.x path allocates an mfxLoader; the legacy
        // MSDK path's MFXInit doesn't have a loader to unload.
        if (loader_ && L.MFXUnload) {
            L.MFXUnload(loader_);
            loader_ = nullptr;
        }
    }

    Config cfg_{};
    mfxLoader loader_ = nullptr;
    mfxSession session_ = nullptr;
    mfxVideoParam vpar_{};
    mfxBitstream bs_{};
    int num_surfaces_needed_ = 0;
    bool encode_init_done_ = false;
    // True while we want to try MFXMemory_GetSurfaceForEncode. Set to
    // false on first failure (1.x-compat shim returns INVALID_HANDLE).
    bool use_frame_interface_ = true;
    std::atomic<bool> force_idr_{true};

    std::vector<mfxFrameSurface1> surfaces_;    // system-memory NV12 pool
    std::vector<std::uint8_t>     surface_buf_; // backing storage
    std::vector<std::uint8_t>     bs_buf_;      // encoded bitstream buffer
};

}  // namespace

std::unique_ptr<Encoder> MakeOneVplEncoder() {
    auto& L = Loader();
    if (!L.ready) {
        std::fprintf(stderr,
            "unio-pipe: oneVPL factory declining — %s\n",
            L.reason.c_str());
        return nullptr;
    }
    return std::make_unique<OneVplEncoder>();
}

}  // namespace unio

#endif  // _WIN32
