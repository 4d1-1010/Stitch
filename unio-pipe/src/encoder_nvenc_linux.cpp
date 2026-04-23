// NVENC H.264 encoder (Linux) — sibling of encoder_nvenc.cpp.
// Consumes BGRX CpuFrames from XComposite capture, emits
// Annex-B H.264 NAL bytes. Uses CUDA input surfaces instead of
// the D3D11 path the Windows sibling uses.
//
// Runtime: libnvidia-encode.so.1 (loaded via ffnvcodec's
// NvEncodeAPICreateInstance on first use) + libcuda.so.1 (loaded
// by CudaRuntime). Both ship with the NVIDIA proprietary driver;
// zero build-time dependency on NVIDIA's SDK.
//
// Buffer path:
//   CpuFrame BGRX (CPU)
//     ─ cuMemcpy2D HtoD ─►  CUDA device buffer (ARGB / BGRA byte order)
//     ─ NvEncRegisterResource(CUDA) + Map ─►  NV_ENC_INPUT_PTR
//     ─ nvEncEncodePicture ─►  bitstream
//     ─ LockBitstream ─►  copy to EncodedPacket + Annex-B SEI prefix
//     ─ UnmapInputResource ─►  CUDA buffer free after stream close
//
// Zero-copy XComposite → CUDA would be the follow-up: capture
// into a GL texture or DMA-BUF + CUDA graphics interop skips the
// HtoD memcpy. First pass stays CPU-copy to get the path working
// on adi-pc; matches encoder_vaapi's BGRX→NV12 CPU-upload shape.

#if !defined(__linux__)
#include "encoder.h"
namespace unio {
std::unique_ptr<Encoder> MakeNvencLinuxEncoder() { return nullptr; }
}  // namespace unio
#else

#include "encoder.h"
#include "cuda_runtime.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <dlfcn.h>

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/nvEncodeAPI.h>

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count());
}

// Singleton loader for libnvidia-encode.so.1. ffnvcodec's
// NVENC loader helper (nvenc_load_functions) is geared for
// Windows's fully-defined NV_ENCODE_API_FUNCTION_LIST, which we
// use directly via NvEncodeAPICreateInstance. The result is a
// NV_ENCODE_API_FUNCTION_LIST with every entry filled in — same
// approach as the Windows NVENC TU, just against libnvidia-
// encode.so.1 via dlopen.
//
// Not wrapped in CudaRuntime because CUDA + NVENC are independent
// shared libraries on Linux — the driver ships both and either
// can be missing on a broken install (rare but flagged in #43).
using PNVENCODEAPICREATEINSTANCE =
    NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

struct NvencLoader {
    void* lib = nullptr;
    PNVENCODEAPICREATEINSTANCE create = nullptr;
    std::string reason;
    bool ready = false;
};

NvencLoader& NvencLib() {
    static NvencLoader loader;
    static std::once_flag flag;
    std::call_once(flag, []() {
        loader.lib = dlopen("libnvidia-encode.so.1",
                             RTLD_NOW | RTLD_LOCAL);
        if (!loader.lib) {
            loader.reason = "libnvidia-encode.so.1 not loadable";
            return;
        }
        loader.create = reinterpret_cast<PNVENCODEAPICREATEINSTANCE>(
            dlsym(loader.lib, "NvEncodeAPICreateInstance"));
        if (!loader.create) {
            loader.reason = "NvEncodeAPICreateInstance symbol missing";
            return;
        }
        loader.ready = true;
    });
    return loader;
}

class NvencLinuxEncoder final : public Encoder {
public:
    NvencLinuxEncoder() = default;
    ~NvencLinuxEncoder() override { Teardown(); }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;
        auto& cuda_rt = CudaRuntime::Instance();
        if (!cuda_rt.ready()) {
            return std::string("CUDA runtime not ready: ")
                   + cuda_rt.reason();
        }
        cuda_ = cuda_rt.cuda_fns();
        cu_ctx_ = cuda_rt.primary_ctx();

        auto& loader = NvencLib();
        if (!loader.ready) return loader.reason;
        nv_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (loader.create(&nv_) != NV_ENC_SUCCESS) {
            return "NvEncodeAPICreateInstance failed";
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
        open.device = cu_ctx_;
        open.apiVersion = NVENCAPI_VERSION;
        if (nv_.nvEncOpenEncodeSessionEx(&open, &session_)
            != NV_ENC_SUCCESS || !session_) {
            return "nvEncOpenEncodeSessionEx(CUDA) failed";
        }

        // Same preset + tune + CQP settings as the Windows NVENC
        // path (encoder_nvenc.cpp). PR 7 Day 1 settled on P4 +
        // LOW_LATENCY; PR 9 Day 5 dropped QP by 5 vs Linux VA-API
        // to absorb generational loss on already-compressed
        // content. Keeping the numbers identical across OSes
        // means the sink sees bit-identical bitstreams regardless
        // of which vendor encoded them, minus resolution/motion
        // content itself.
        NV_ENC_PRESET_CONFIG preset{};
        preset.version = NV_ENC_PRESET_CONFIG_VER;
        preset.presetCfg.version = NV_ENC_CONFIG_VER;
        if (nv_.nvEncGetEncodePresetConfigEx(
                session_, NV_ENC_CODEC_H264_GUID,
                NV_ENC_PRESET_P4_GUID,
                NV_ENC_TUNING_INFO_LOW_LATENCY, &preset)
            != NV_ENC_SUCCESS) {
            return "nvEncGetEncodePresetConfigEx failed";
        }

        NV_ENC_CONFIG ec = preset.presetCfg;
        ec.profileGUID = NV_ENC_H264_PROFILE_MAIN_GUID;
        ec.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
        const int nvenc_qp = std::max(cfg.quality - 5, 10);
        ec.rcParams.constQP.qpIntra = nvenc_qp;
        ec.rcParams.constQP.qpInterP = nvenc_qp;
        ec.rcParams.constQP.qpInterB = nvenc_qp;
        ec.gopLength = NVENC_INFINITE_GOPLENGTH;
        ec.frameIntervalP = 1;  // IPPP
        ec.encodeCodecConfig.h264Config.idrPeriod =
            NVENC_INFINITE_GOPLENGTH;
        ec.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        ec.encodeCodecConfig.h264Config.outputAUD = 0;

        NV_ENC_INITIALIZE_PARAMS ip{};
        ip.version = NV_ENC_INITIALIZE_PARAMS_VER;
        ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
        ip.presetGUID = NV_ENC_PRESET_P4_GUID;
        ip.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
        ip.encodeWidth = cfg.width;
        ip.encodeHeight = cfg.height;
        ip.darWidth = cfg.width;
        ip.darHeight = cfg.height;
        ip.frameRateNum = cfg.fps;
        ip.frameRateDen = 1;
        ip.enablePTD = 1;
        ip.encodeConfig = &ec;
        if (nv_.nvEncInitializeEncoder(session_, &ip)
            != NV_ENC_SUCCESS) {
            return "nvEncInitializeEncoder failed";
        }

        // CUDA input buffer. ARGB format at cfg.width * 4 pitch;
        // allocated with cuMemAllocPitch so the driver picks a
        // hardware-friendly row stride. Registered with NVENC
        // once and reused for every frame — CUDA device pointers
        // are stable within the CUcontext lifetime so one
        // registration suffices.
        cuda_->cuCtxPushCurrent(cu_ctx_);
        std::size_t pitch = 0;
        const int row_bytes = cfg.width * 4;
        if (cuda_->cuMemAllocPitch(&cuda_input_, &pitch,
                                    row_bytes, cfg.height, 16) != 0) {
            CUcontext popped{};
            cuda_->cuCtxPopCurrent(&popped);
            return "cuMemAllocPitch failed (CUDA input buffer)";
        }
        cuda_input_pitch_ = static_cast<std::uint32_t>(pitch);
        CUcontext popped{};
        cuda_->cuCtxPopCurrent(&popped);

        NV_ENC_REGISTER_RESOURCE rr{};
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
        rr.width = cfg.width;
        rr.height = cfg.height;
        rr.pitch = cuda_input_pitch_;
        rr.resourceToRegister =
            reinterpret_cast<void*>(cuda_input_);
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (nv_.nvEncRegisterResource(session_, &rr)
            != NV_ENC_SUCCESS) {
            return "nvEncRegisterResource(CUDA) failed";
        }
        registered_ = rr.registeredResource;

        NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (nv_.nvEncCreateBitstreamBuffer(session_, &bb)
            != NV_ENC_SUCCESS) {
            return "nvEncCreateBitstreamBuffer failed";
        }
        bitstream_buffer_ = bb.bitstreamBuffer;

        std::fprintf(stderr,
            "unio-pipe: NVENC-Linux initialized %dx%d@%dfps, "
            "H.264 Main, CQP %d, CUDA pitch=%u, ctx=%p\n",
            cfg.width, cfg.height, cfg.fps, nvenc_qp,
            cuda_input_pitch_, static_cast<void*>(cu_ctx_));
        initialized_ = true;
        return std::nullopt;
    }

    void ForceIdr() override {
        force_idr_.store(true, std::memory_order_release);
    }

    EncodedPacketPtr Encode(const CpuFrame& frame) override {
        if (!initialized_) return nullptr;

        // CPU→CUDA memcpy. XComposite emits BGRX at frame.stride_bytes;
        // copy only the rows we have, clipped to cfg_.height.
        const std::uint32_t rows = std::min<std::uint32_t>(
            frame.height, static_cast<std::uint32_t>(cfg_.height));
        const std::uint32_t row_bytes = std::min<std::uint32_t>(
            frame.stride_bytes,
            static_cast<std::uint32_t>(cfg_.width) * 4);

        cuda_->cuCtxPushCurrent(cu_ctx_);
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_HOST;
        m.srcHost = frame.pixels.data();
        m.srcPitch = frame.stride_bytes;
        m.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        m.dstDevice = cuda_input_;
        m.dstPitch = cuda_input_pitch_;
        m.WidthInBytes = row_bytes;
        m.Height = rows;
        const int cuda_ret = cuda_->cuMemcpy2D(&m);
        CUcontext popped{};
        cuda_->cuCtxPopCurrent(&popped);
        if (cuda_ret != 0) {
            std::fprintf(stderr,
                "unio-pipe: NVENC-Linux cuMemcpy2D HtoD failed "
                "(CUresult=%d)\n", cuda_ret);
            return nullptr;
        }

        // Map the registered CUDA input resource to an NVENC input
        // pointer. Needed before every encode; unmap afterwards.
        // Cheap — the driver already knows the CUdeviceptr.
        NV_ENC_MAP_INPUT_RESOURCE mr{};
        mr.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mr.registeredResource = registered_;
        if (nv_.nvEncMapInputResource(session_, &mr)
            != NV_ENC_SUCCESS) {
            return nullptr;
        }
        auto pkt = EncodeMapped(mr.mappedResource,
                                 cuda_input_pitch_,
                                 frame.frame_id,
                                 frame.capture_monotonic_ns);
        nv_.nvEncUnmapInputResource(session_, mr.mappedResource);
        return pkt;
    }

    std::string_view Name() const override { return "nvenc-linux"; }

private:
    EncodedPacketPtr EncodeMapped(NV_ENC_INPUT_PTR input,
                                   std::uint32_t pitch,
                                   std::uint64_t frame_id,
                                   std::uint64_t capture_ns) {
        NV_ENC_PIC_PARAMS pp{};
        pp.version = NV_ENC_PIC_PARAMS_VER;
        pp.inputWidth = cfg_.width;
        pp.inputHeight = cfg_.height;
        pp.inputPitch = pitch;
        pp.inputBuffer = input;
        pp.outputBitstream = bitstream_buffer_;
        pp.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pp.frameIdx = static_cast<std::uint32_t>(frame_count_);
        pp.inputTimeStamp = capture_ns;
        const bool is_idr = force_idr_.exchange(false,
                std::memory_order_acq_rel);
        if (is_idr) {
            pp.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR
                              | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            pp.pictureType = NV_ENC_PIC_TYPE_IDR;
        } else {
            pp.pictureType = NV_ENC_PIC_TYPE_P;
        }
        static int log_n = 0;
        if (log_n++ < 3 || is_idr) {
            std::fprintf(stderr,
                "unio-pipe: nvenc-linux encode frame %llu idr=%d\n",
                static_cast<unsigned long long>(frame_count_),
                is_idr ? 1 : 0);
            std::fflush(stderr);
        }

        auto st = nv_.nvEncEncodePicture(session_, &pp);
        if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
            ++frame_count_;
            auto skip = std::make_unique<EncodedPacket>();
            skip->frame_id = frame_id;
            skip->capture_monotonic_ns = capture_ns;
            return skip;
        }
        if (st != NV_ENC_SUCCESS) {
            std::fprintf(stderr,
                "unio-pipe: nvenc-linux encode failed: %d\n",
                static_cast<int>(st));
            return nullptr;
        }

        NV_ENC_LOCK_BITSTREAM lb{};
        lb.version = NV_ENC_LOCK_BITSTREAM_VER;
        lb.outputBitstream = bitstream_buffer_;
        lb.doNotWait = 0;
        if (nv_.nvEncLockBitstream(session_, &lb) != NV_ENC_SUCCESS) {
            return nullptr;
        }

        auto pkt = std::make_unique<EncodedPacket>();
        pkt->frame_id = frame_id;
        pkt->capture_monotonic_ns = capture_ns;
        pkt->key_frame = (lb.pictureType == NV_ENC_PIC_TYPE_IDR);
        auto sei = BuildLatencySeiAnnexB(pkt->frame_id,
                                          pkt->capture_monotonic_ns);
        pkt->nal_bytes.reserve(sei.size() + lb.bitstreamSizeInBytes);
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                               sei.begin(), sei.end());
        const auto* bits = static_cast<const std::uint8_t*>(
            lb.bitstreamBufferPtr);
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                               bits, bits + lb.bitstreamSizeInBytes);
        pkt->encode_done_monotonic_ns = NowNs();
        nv_.nvEncUnlockBitstream(session_, bitstream_buffer_);

        ++frame_count_;
        return pkt;
    }

    void Teardown() {
        // Per-field guards (same style as NvdecDecoder::Teardown):
        // each handle cleans up independently so a partially
        // successful Init() — e.g. session created but
        // bitstream_buffer allocation failed — doesn't leak the
        // resources that did get far enough to be valid.
        if (session_ && registered_) {
            nv_.nvEncUnregisterResource(session_, registered_);
            registered_ = nullptr;
        }
        if (session_ && bitstream_buffer_) {
            nv_.nvEncDestroyBitstreamBuffer(session_, bitstream_buffer_);
            bitstream_buffer_ = nullptr;
        }
        if (cuda_input_ && cuda_) {
            cuda_->cuCtxPushCurrent(cu_ctx_);
            cuda_->cuMemFree(cuda_input_);
            CUcontext popped{};
            cuda_->cuCtxPopCurrent(&popped);
            cuda_input_ = 0;
        }
        if (session_) {
            nv_.nvEncDestroyEncoder(session_);
            session_ = nullptr;
        }
        initialized_ = false;
    }

    // State
    Config cfg_{};
    NV_ENCODE_API_FUNCTION_LIST nv_{};
    void* session_ = nullptr;           // NV_ENC_HANDLE
    CudaFunctions* cuda_ = nullptr;     // not owned (CudaRuntime)
    CUcontext cu_ctx_ = nullptr;        // not owned
    CUdeviceptr cuda_input_ = 0;
    std::uint32_t cuda_input_pitch_ = 0;
    void* registered_ = nullptr;        // NV_ENC_REGISTERED_PTR
    void* bitstream_buffer_ = nullptr;  // NV_ENC_OUTPUT_PTR
    std::uint64_t frame_count_ = 0;
    std::atomic<bool> force_idr_{true}; // true → first frame is IDR
    bool initialized_ = false;
};

}  // namespace

std::unique_ptr<Encoder> MakeNvencLinuxEncoder() {
    if (!CudaRuntime::Instance().ready()) {
        std::fprintf(stderr,
            "unio-pipe: NVENC-Linux factory declining — %s\n",
            CudaRuntime::Instance().reason().c_str());
        return nullptr;
    }
    if (!NvencLib().ready) {
        std::fprintf(stderr,
            "unio-pipe: NVENC-Linux factory declining — %s\n",
            NvencLib().reason.c_str());
        return nullptr;
    }
    return std::make_unique<NvencLinuxEncoder>();
}

}  // namespace unio

#endif  // __linux__
