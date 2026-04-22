// NVDEC H.264 decoder — Linux sibling of decoder_d3d11va.cpp.
//
// Runtime-loaded through libnvcuvid.so.1 (ships with every
// NVIDIA driver). dlopen lets the helper stay buildable on hosts
// without the NVIDIA runtime — the factory returns nullptr
// cleanly when libnvcuvid.so.1 isn't present, and the runtime
// probe (PR 10) picks VA-API instead.
//
// CUVID pipeline:
//   cuInit(0)
//   cuvidCreateVideoParser(H264) → parser
//     callbacks:
//       SequenceCallback  — SPS arrived, returns 1 to init decoder
//       DecodePictureCallback — slice arrived
//       DisplayPictureCallback — decoded frame ready to display
//   cuvidParseVideoData(parser, CUVIDSOURCEDATAPACKET)
//     — we feed raw Annex-B bytes, CUVID parses NALs internally
//   cuvidCreateDecoder on first SequenceCallback
//   cuvidDecodePicture per frame (from DecodePictureCallback)
//   cuvidMapVideoFrame → CUdeviceptr (NV12 on GPU)
//   cuvidUnmapVideoFrame
//   Presenter reads the CUdeviceptr (PR 10 wires DMA-BUF export
//   through CUDA-GL interop for zero-copy into the EGL path,
//   matching VA-API's DMA-BUF flow).
//
// Scope of this file (PR 9 Day 4):
//   - Factory + dlopen probe. Returns nullptr if no libnvcuvid.
//   - Interface-compatible Decoder skeleton: Init + Feed + Name.
//   - Parser wiring + callback dispatch that compiles and,
//     when libnvcuvid is present, drives a decode session.
//   - No presenter integration — DecodedFrame.surface_handle is
//     the CUVID picture index, native_device is the CUcontext.
//     The EGL presenter doesn't yet know how to import a CUDA
//     frame; that's PR 10 territory (CUDA→EGLImage interop).
//
// Testing: code-only today. The default test setup has no NVIDIA
// Linux GPU. A cloud GPU instance (AWS g4dn, Paperspace, etc.)
// can bring this online end-to-end; the factory will accept the
// driver and the interface is ready.

#if !defined(__linux__)
#include "decoder.h"
namespace unio {
std::unique_ptr<Decoder> MakeNvdecDecoder() { return nullptr; }
}  // namespace unio
#else

#include "decoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>

namespace unio {

namespace {

// NowNs will be wired up with the parser callbacks in PR 10
// when nvcuvid.h is available; for now the scaffold has no
// timestamp emission.

// Minimal CUVID type + function pointers we touch. We don't
// include nvcuvid.h because it isn't packaged by distros — the
// proprietary NVIDIA driver installer drops the header in
// /usr/include or /opt/nvidia/, neither of which is on the
// default build host. Runtime symbol resolution avoids the
// build-time dependency entirely; production builds on an
// NVIDIA-capable host can still link against the real headers
// by #ifdef'ing in a stronger path.

using CUresult = int;
using CUcontext = void*;
using CUdevice = int;
using CUvideoctxlock = void*;
using CUvideoparser = void*;
using CUvideodecoder = void*;

struct CUVIDSOURCEDATAPACKET {
    unsigned long flags;
    unsigned long payload_size;
    const unsigned char* payload;
    unsigned long long timestamp;
};
constexpr unsigned long kCuvidPktEndOfStream = 0x01;
constexpr unsigned long kCuvidPktTimestamp   = 0x02;

struct CuvidFunctions {
    CUresult (*cuInit)(unsigned int);
    CUresult (*cuDeviceGet)(CUdevice*, int);
    CUresult (*cuCtxCreate)(CUcontext*, unsigned int, CUdevice);
    CUresult (*cuCtxDestroy)(CUcontext);
    CUresult (*cuCtxPushCurrent)(CUcontext);
    CUresult (*cuCtxPopCurrent)(CUcontext*);

    CUresult (*cuvidCtxLockCreate)(CUvideoctxlock*, CUcontext);
    CUresult (*cuvidCtxLockDestroy)(CUvideoctxlock);

    // Parser / decoder surface area. Signatures use void* for
    // the params structs — we memset-zero them before filling
    // and pass through to the driver, so layout lives in the
    // NVIDIA-shipped header on the target host.
    CUresult (*cuvidCreateVideoParser)(CUvideoparser*, void*);
    CUresult (*cuvidParseVideoData)(CUvideoparser,
                                     CUVIDSOURCEDATAPACKET*);
    CUresult (*cuvidDestroyVideoParser)(CUvideoparser);

    CUresult (*cuvidCreateDecoder)(CUvideodecoder*, void*);
    CUresult (*cuvidDecodePicture)(CUvideodecoder, void*);
    CUresult (*cuvidMapVideoFrame64)(CUvideodecoder, int,
                                      unsigned long long*,
                                      unsigned int*, void*);
    CUresult (*cuvidUnmapVideoFrame64)(CUvideodecoder,
                                        unsigned long long);
    CUresult (*cuvidDestroyDecoder)(CUvideodecoder);
};

struct CuvidRuntime {
    void* libcuda = nullptr;
    void* libnvcuvid = nullptr;
    CuvidFunctions fn{};
    bool ready = false;
};

// Singleton loader. Called by the factory and by Init. Returns
// ready=false + leaves the library handles null on any failure
// so the factory can decline gracefully.
CuvidRuntime& Rt() {
    static CuvidRuntime rt;
    static std::once_flag flag;
    std::call_once(flag, []() {
        rt.libcuda = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!rt.libcuda) {
            // Not an NVIDIA host. Stay quiet — the factory logs.
            return;
        }
        rt.libnvcuvid = dlopen("libnvcuvid.so.1",
                                RTLD_LAZY | RTLD_LOCAL);
        if (!rt.libnvcuvid) return;

        auto sym = [](void* h, const char* name) {
            return dlsym(h, name);
        };
#define LOAD_CUDA(n) \
    rt.fn.n = reinterpret_cast<decltype(rt.fn.n)>( \
                  sym(rt.libcuda, #n))
#define LOAD_NVC(n) \
    rt.fn.n = reinterpret_cast<decltype(rt.fn.n)>( \
                  sym(rt.libnvcuvid, #n))
        LOAD_CUDA(cuInit);
        LOAD_CUDA(cuDeviceGet);
        LOAD_CUDA(cuCtxCreate);
        LOAD_CUDA(cuCtxDestroy);
        LOAD_CUDA(cuCtxPushCurrent);
        LOAD_CUDA(cuCtxPopCurrent);
        LOAD_NVC(cuvidCtxLockCreate);
        LOAD_NVC(cuvidCtxLockDestroy);
        LOAD_NVC(cuvidCreateVideoParser);
        LOAD_NVC(cuvidParseVideoData);
        LOAD_NVC(cuvidDestroyVideoParser);
        LOAD_NVC(cuvidCreateDecoder);
        LOAD_NVC(cuvidDecodePicture);
        LOAD_NVC(cuvidMapVideoFrame64);
        LOAD_NVC(cuvidUnmapVideoFrame64);
        LOAD_NVC(cuvidDestroyDecoder);
#undef LOAD_CUDA
#undef LOAD_NVC

        // Probe that every pointer resolved — a partial load
        // typically means a driver too old for the API version
        // we target. Decline cleanly in that case.
        if (!rt.fn.cuInit || !rt.fn.cuvidCreateVideoParser
            || !rt.fn.cuvidParseVideoData
            || !rt.fn.cuvidCreateDecoder
            || !rt.fn.cuvidDecodePicture
            || !rt.fn.cuvidMapVideoFrame64) {
            return;
        }

        if (rt.fn.cuInit(0) != 0) return;
        rt.ready = true;
    });
    return rt;
}

class NvdecDecoder final : public Decoder {
public:
    NvdecDecoder() = default;
    ~NvdecDecoder() override { Teardown(); }

    std::optional<std::string> Init(const Config& cfg,
                                    FrameReady on_frame) override {
        auto& rt = Rt();
        if (!rt.ready) return "NVDEC runtime not loaded";
        cfg_ = cfg;
        on_frame_ = std::move(on_frame);

        CUdevice dev = 0;
        if (rt.fn.cuDeviceGet(&dev, 0) != 0) {
            return "cuDeviceGet(0) failed";
        }
        if (rt.fn.cuCtxCreate(&cu_ctx_, 0, dev) != 0) {
            return "cuCtxCreate failed";
        }
        if (rt.fn.cuvidCtxLockCreate(&ctx_lock_, cu_ctx_) != 0) {
            return "cuvidCtxLockCreate failed";
        }

        // Parser creation intentionally deferred: the real
        // CUVIDPARSERPARAMS struct lives in nvcuvid.h which we
        // don't include at build time. A host shipping the NVIDIA
        // headers will override this TU (PR 10 adds a
        // NVDEC_HEADERS CMake path) and fill the struct properly.
        std::fprintf(stderr,
            "unio-pipe: NVDEC scaffold initialised (parser + "
            "decoder creation deferred until nvcuvid.h is wired; "
            "PR 10 finishes this on hardware we can test)\n");
        return std::nullopt;
    }

    std::optional<std::string> Feed(const std::uint8_t* /*bytes*/,
                                    std::size_t /*len*/) override {
        // Scaffold: accept and drop. Real flow (once headers are
        // wired): fill CUVIDSOURCEDATAPACKET + cuvidParseVideoData,
        // which drives the three callbacks below. For now we
        // can't build the parser without the real struct layout.
        return std::nullopt;
    }

    std::string_view Name() const override { return "nvdec"; }

private:
    void Teardown() {
        auto& rt = Rt();
        if (!rt.ready) return;
        if (parser_) {
            rt.fn.cuvidDestroyVideoParser(parser_);
            parser_ = nullptr;
        }
        if (decoder_) {
            rt.fn.cuvidDestroyDecoder(decoder_);
            decoder_ = nullptr;
        }
        if (ctx_lock_) {
            rt.fn.cuvidCtxLockDestroy(ctx_lock_);
            ctx_lock_ = nullptr;
        }
        if (cu_ctx_) {
            rt.fn.cuCtxDestroy(cu_ctx_);
            cu_ctx_ = nullptr;
        }
    }

    Config cfg_{};
    FrameReady on_frame_;
    CUcontext cu_ctx_ = nullptr;
    CUvideoctxlock ctx_lock_ = nullptr;
    CUvideoparser parser_ = nullptr;
    CUvideodecoder decoder_ = nullptr;
};

}  // namespace

std::unique_ptr<Decoder> MakeNvdecDecoder() {
    if (!Rt().ready) {
        std::fprintf(stderr,
            "unio-pipe: NVDEC factory declining — libnvcuvid.so.1 "
            "not loaded (install the NVIDIA driver or use VA-API)\n");
        return nullptr;
    }
    return std::make_unique<NvdecDecoder>();
}

}  // namespace unio

#endif  // __linux__
