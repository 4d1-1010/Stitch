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

// Runtime loader for libvpl.dll / libmfx.dll — the set of entry
// points we call from this TU. Filled in on first access via the
// standard oneVPL 2.x dispatcher symbols (MFXLoad, MFXUnload,
// MFXCreateSession, etc.). Missing symbols leave .ready=false so
// the factory declines cleanly.
//
// Kept in a singleton so the decoder TU (decoder_onevpl.cpp) can
// share the same loader state in step 3a of the plan.
struct OneVplLoader {
    HMODULE lib = nullptr;
    // Dispatcher
    mfxLoader (MFX_CDECL *MFXLoad)(void) = nullptr;
    void (MFX_CDECL *MFXUnload)(mfxLoader) = nullptr;
    mfxConfig (MFX_CDECL *MFXCreateConfig)(mfxLoader) = nullptr;
    mfxStatus (MFX_CDECL *MFXSetConfigFilterProperty)(
        mfxConfig, const mfxU8*, mfxVariant) = nullptr;
    mfxStatus (MFX_CDECL *MFXCreateSession)(
        mfxLoader, mfxU32, mfxSession*) = nullptr;
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
    // internal-alloc surface pool.
    mfxStatus (MFX_CDECL *MFXMemory_GetSurfaceForEncode)(
        mfxSession, mfxFrameSurface1**) = nullptr;

    std::string reason;
    bool ready = false;
};

OneVplLoader& Loader() {
    static OneVplLoader L;
    static std::once_flag flag;
    std::call_once(flag, []() {
        // libvpl.dll first (modern oneVPL 2.x drivers), fall back
        // to libmfx.dll (legacy MSDK drivers). The dispatcher
        // symbols we resolve are the same in both.
        L.lib = LoadLibraryW(L"libvpl.dll");
        if (!L.lib) L.lib = LoadLibraryW(L"libmfx.dll");
        if (!L.lib) {
            L.reason = "libvpl.dll / libmfx.dll not loadable "
                       "(no Intel graphics driver?)";
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
        LOAD_SYM(MFXClose);
        LOAD_SYM(MFXVideoENCODE_Query);
        LOAD_SYM(MFXVideoENCODE_QueryIOSurf);
        LOAD_SYM(MFXVideoENCODE_Init);
        LOAD_SYM(MFXVideoENCODE_Close);
        LOAD_SYM(MFXVideoENCODE_EncodeFrameAsync);
        LOAD_SYM(MFXVideoCORE_SyncOperation);
        LOAD_SYM(MFXMemory_GetSurfaceForEncode);
        #undef LOAD_SYM

        // The four dispatcher symbols are mandatory for step 2a.
        // The encode + sync symbols get used in later commits.
        if (!L.MFXLoad || !L.MFXCreateConfig
            || !L.MFXSetConfigFilterProperty
            || !L.MFXCreateSession || !L.MFXClose) {
            L.reason = "libvpl dispatcher symbols missing "
                       "(driver too old for oneVPL 2.x API?)";
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

        loader_ = L.MFXLoad();
        if (!loader_) return "MFXLoad returned null";

        // Filter 1: implementation type = hardware (avoid software
        // fallback — we want the iGPU's media engine, not x86
        // code. oneVPL will pick sw if no matching hw is found;
        // we decline that via step 2b's MFXVideoENCODE_Query.)
        mfxConfig cfg1 = L.MFXCreateConfig(loader_);
        if (!cfg1) return "MFXCreateConfig #1 failed";
        if (auto s = SetFilter(L, cfg1,
                "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE);
            s != MFX_ERR_NONE) {
            return std::string("SetFilter Impl=HARDWARE failed: ")
                   + std::to_string(s);
        }

        // Filter 2: codec = H.264 (AVC).
        mfxConfig cfg2 = L.MFXCreateConfig(loader_);
        if (!cfg2) return "MFXCreateConfig #2 failed";
        if (auto s = SetFilter(L, cfg2,
                "mfxImplDescription.mfxEncoderDescription."
                "encoder.CodecID", MFX_CODEC_AVC);
            s != MFX_ERR_NONE) {
            return std::string("SetFilter CodecID=AVC failed: ")
                   + std::to_string(s);
        }

        // Filter 3: API version >= 2.0 (oneVPL 2.x).
        mfxConfig cfg3 = L.MFXCreateConfig(loader_);
        if (!cfg3) return "MFXCreateConfig #3 failed";
        mfxVariant ver{};
        ver.Version.Version = MFX_VARIANT_VERSION;
        ver.Type = MFX_VARIANT_TYPE_U32;
        // API 2.0 encoded as 0x00020000; 2.x any minor is fine.
        ver.Data.U32 = (2U << 16);
        if (auto s = L.MFXSetConfigFilterProperty(cfg3,
                reinterpret_cast<const mfxU8*>(
                    "mfxImplDescription.ApiVersion.Version"), ver);
            s != MFX_ERR_NONE) {
            return std::string("SetFilter ApiVersion=2.x failed: ")
                   + std::to_string(s);
        }

        // Create the session from the filter chain. adapter_num=0
        // picks the first matching implementation — on Diana that's
        // Intel UHD (the iGPU with Quick Sync). Multi-adapter
        // selection (Intel vs NVIDIA) is a follow-up that pairs
        // with the render-node enumeration work (#44).
        if (auto s = L.MFXCreateSession(loader_, 0, &session_);
            s != MFX_ERR_NONE || !session_) {
            return std::string("MFXCreateSession failed: ")
                   + std::to_string(s);
        }

        std::fprintf(stderr,
            "unio-pipe: oneVPL session open — H.264 HW encoder, "
            "session=%p, loader=%p\n",
            static_cast<void*>(session_),
            static_cast<void*>(loader_));

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
        if (loader_) {
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
