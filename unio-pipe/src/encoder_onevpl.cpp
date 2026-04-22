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
//   2a. Session bring-up only — MFXLoad → filter chain → MFXCreateSession.
//   2b. Query + QueryIOSurf + MFXVideoENCODE_Init.
//   2c. First real encode — internal surface alloc + EncodeFrameAsync + SyncOperation.
//   2d. Real per-frame loop with IDR + latency SEI prefix.
//
// This file in commit 2a/N: steps 2a only. Encode() returns
// nullptr with a log line until 2c lands.

#if !defined(_WIN32)
#include "encoder.h"
namespace unio {
std::unique_ptr<Encoder> MakeOneVplEncoder() { return nullptr; }
}  // namespace unio
#else

#include "encoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

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

        // Step 2b+ land the actual MFXVideoENCODE_Init. For 2a we
        // stop here — the session is open + the factory returns
        // a real encoder, but Encode() declines until the init
        // path is wired.
        session_open_only_ = true;
        return std::nullopt;
    }

    void ForceIdr() override {
        force_idr_.store(true, std::memory_order_release);
    }

    EncodedPacketPtr Encode(const CpuFrame& frame) override {
        if (session_open_only_) {
            static std::once_flag warn_once;
            std::call_once(warn_once, []() {
                std::fprintf(stderr,
                    "unio-pipe: oneVPL encoder Encode() not yet "
                    "implemented (step 2a lands session only; "
                    "step 2c adds the real encode loop). #26\n");
            });
            (void)frame;
            return nullptr;
        }
        // Step 2c fills this in.
        return nullptr;
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
    std::atomic<bool> force_idr_{true};
    bool session_open_only_ = false;
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
