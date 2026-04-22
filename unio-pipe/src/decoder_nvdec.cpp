// NVDEC H.264 decoder — Linux sibling of decoder_d3d11va.cpp.
//
// Uses CUVID (libnvcuvid.so.1) through ffnvcodec/dynlink_loader.h's
// cuvid_load_functions helper. CUDA context is shared via
// CudaRuntime (src/cuda_runtime.cpp) so both NVENC (#27) and
// NVDEC live on the same primary context — GPU memory they
// allocate is addressable from either side, which is the
// prerequisite for zero-copy CUDA → GL interop later.
//
// Pipeline (WP 10 Linux NVIDIA, #21):
//   cuvidCreateVideoParser(H264)  →  parser
//     callbacks:
//       SequenceCallback    — SPS parsed, decide decoder size
//       DecodePictureCallback — slice arrived, decode
//       DisplayPictureCallback — frame ready, CUDA→CPU copy, emit
//   cuvidParseVideoData(parser, CUVIDSOURCEDATAPACKET)
//     — we feed raw Annex-B bytes (with start codes), CUVID
//     internally splits NALs.
//   cuvidCreateDecoder on first SequenceCallback.
//   cuvidMapVideoFrame in DisplayPictureCallback → CUdeviceptr.
//   cuMemcpy2D to a CPU-owned NV12 buffer → emit DecodedFrame
//     with source_kind=CpuNv12. (Zero-copy CUDA→EGL interop is a
//     follow-up; this PR's goal is get-bytes-flowing on adi-pc.)
//   cuvidUnmapVideoFrame before returning to the driver.
//
// Frame buffer lifetime: a small ring of CPU NV12 buffers (8
// slots × frame-sized). Each DisplayPictureCallback writes into
// the next slot, calls on_frame_ synchronously with a pointer
// into it, and advances. With InboundStream's SPSC depth 2-4 and
// 8-slot ring, we won't overwrite an in-flight frame even at
// 60 fps with p95 presenter stalls. If this assumption breaks,
// we move to reference-counted ownership.

#if !defined(__linux__)
#include "decoder.h"
namespace unio {
std::unique_ptr<Decoder> MakeNvdecDecoder() { return nullptr; }
}  // namespace unio
#else

#include "decoder.h"
#include "cuda_runtime.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <ffnvcodec/dynlink_cuda.h>
#include <ffnvcodec/dynlink_cuviddec.h>
#include <ffnvcodec/dynlink_nvcuvid.h>
#include <ffnvcodec/dynlink_loader.h>

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count());
}

// Singleton loader for libnvcuvid. CudaFunctions lives in
// CudaRuntime; CUVID is independent — the driver ships both but
// they're separate shared libraries and separate symbol tables.
CuvidFunctions* LoadCuvid(std::string* reason_out) {
    CuvidFunctions* fns = nullptr;
    const int r = cuvid_load_functions(&fns, nullptr);
    if (r != 0 || !fns) {
        if (reason_out) *reason_out = "libnvcuvid.so.1 not loadable";
        return nullptr;
    }
    return fns;
}

}  // namespace

class NvdecDecoder final : public Decoder {
public:
    NvdecDecoder() = default;
    ~NvdecDecoder() override { Teardown(); }

    std::optional<std::string> Init(const Config& cfg,
                                    FrameReady on_frame) override {
        auto& cuda_rt = CudaRuntime::Instance();
        if (!cuda_rt.ready()) {
            return std::string("CUDA runtime not ready: ")
                   + cuda_rt.reason();
        }
        cuda_ = cuda_rt.cuda_fns();
        cu_ctx_ = cuda_rt.primary_ctx();

        std::string reason;
        cuvid_ = LoadCuvid(&reason);
        if (!cuvid_) return reason;

        cfg_ = cfg;
        on_frame_ = std::move(on_frame);

        // One lock object shared across every parser + decoder
        // call; CUVID expects all its operations for a given
        // context to hold this lock. In our single-decoder-thread
        // world it's effectively free (uncontended).
        if (cuvid_->cuvidCtxLockCreate(&ctx_lock_, cu_ctx_) != 0) {
            return "cuvidCtxLockCreate failed";
        }

        // Parser params.  eCodec H264, blocking-style display
        // callbacks, fresh state.  SEI callback stays null —
        // our latency-SEI parsing happens separately on the
        // Annex-B bytes before we hand them to CUVID.
        CUVIDPARSERPARAMS pp{};
        pp.CodecType = cudaVideoCodec_H264;
        pp.ulMaxNumDecodeSurfaces = kDecodeSurfaceCount;
        pp.ulClockRate = 0;
        pp.ulMaxDisplayDelay = 0;      // low-latency: emit ASAP
        pp.pUserData = this;
        pp.pfnSequenceCallback = &NvdecDecoder::SeqCallback;
        pp.pfnDecodePicture = &NvdecDecoder::DecodeCallback;
        pp.pfnDisplayPicture = &NvdecDecoder::DisplayCallback;

        if (cuvid_->cuvidCreateVideoParser(&parser_, &pp) != 0
            || !parser_) {
            return "cuvidCreateVideoParser(H264) failed";
        }

        std::fprintf(stderr,
            "unio-pipe: NVDEC up, device \"%s\", "
            "ctx=%p parser=%p\n",
            cuda_rt.device_name().c_str(),
            static_cast<void*>(cu_ctx_),
            static_cast<void*>(parser_));
        return std::nullopt;
    }

    std::optional<std::string> Feed(const std::uint8_t* bytes,
                                    std::size_t len) override {
        if (!parser_) return std::string("decoder not initialised");

        // Surface latency-SEI side-channel before handing bytes to
        // CUVID. Matches what decoder_vaapi.cpp does — the driver
        // ignores our UUID'd SEI anyway, but the bytes still flow
        // through so we can pull out frame_id + capture_ns here.
        const auto nals = ScanAnnexB(bytes, len);
        for (const auto& n : nals) {
            if (n.length == 0) continue;
            const std::uint8_t* nal = bytes + n.offset;
            const std::uint8_t type = nal[0] & 0x1F;
            if (type == kNalSei) {
                auto rbsp = StripEmulationPrevention(nal + 1,
                                                      n.length - 1);
                std::uint64_t fid = 0, cap_ns = 0;
                if (ParseLatencySei(rbsp.data(), rbsp.size(),
                                    fid, cap_ns)) {
                    pending_frame_id_ = fid;
                    pending_capture_ns_ = cap_ns;
                }
            }
        }

        CUVIDSOURCEDATAPACKET pkt{};
        pkt.payload = bytes;
        pkt.payload_size = static_cast<unsigned long>(len);
        pkt.flags = 0;
        if (cuvid_->cuvidParseVideoData(parser_, &pkt) != 0) {
            return std::string("cuvidParseVideoData failed");
        }
        return std::nullopt;
    }

    std::string_view Name() const override { return "nvdec"; }

private:
    static constexpr int kDecodeSurfaceCount = 4;
    static constexpr int kCpuRingSlots = 8;

    // ── Static callback trampolines ─────────────────────────────
    //
    // CUVID callbacks are `int (CUDAAPI*)(void*, TYPE*)`. We
    // trampoline into member functions so the per-instance state
    // (frame pool, on_frame_ callback, etc.) is reachable.

    static int CUDAAPI SeqCallback(void* user, CUVIDEOFORMAT* fmt) {
        return static_cast<NvdecDecoder*>(user)
                   ->HandleSequence(fmt);
    }
    static int CUDAAPI DecodeCallback(void* user,
                                       CUVIDPICPARAMS* pic) {
        return static_cast<NvdecDecoder*>(user)
                   ->HandleDecode(pic);
    }
    static int CUDAAPI DisplayCallback(void* user,
                                        CUVIDPARSERDISPINFO* info) {
        return static_cast<NvdecDecoder*>(user)
                   ->HandleDisplay(info);
    }

    // ── Member callbacks ────────────────────────────────────────

    int HandleSequence(CUVIDEOFORMAT* fmt) {
        // Re-create decoder on any size change. The CUVID driver
        // can support cuvidReconfigureDecoder on newer versions,
        // but tearing-and-rebuilding is safer and our streams
        // don't change resolution mid-run.
        if (decoder_ && (width_ != fmt->coded_width
                          || height_ != fmt->coded_height)) {
            cuvid_->cuvidDestroyDecoder(decoder_);
            decoder_ = nullptr;
        }
        width_ = fmt->coded_width;
        height_ = fmt->coded_height;
        // Display area (after cropping). Some H.264 streams have
        // coded > displayed dimensions (e.g. 1920x1080 → 1088
        // coded for MB alignment).
        if (fmt->display_area.right > fmt->display_area.left) {
            disp_width_ = fmt->display_area.right
                          - fmt->display_area.left;
            disp_height_ = fmt->display_area.bottom
                           - fmt->display_area.top;
        } else {
            disp_width_ = width_;
            disp_height_ = height_;
        }

        if (!decoder_) {
            CUVIDDECODECREATEINFO dci{};
            dci.CodecType = cudaVideoCodec_H264;
            dci.ulWidth = fmt->coded_width;
            dci.ulHeight = fmt->coded_height;
            dci.ChromaFormat = cudaVideoChromaFormat_420;
            dci.bitDepthMinus8 = fmt->bit_depth_luma_minus8;
            dci.OutputFormat = cudaVideoSurfaceFormat_NV12;
            dci.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
            dci.ulTargetWidth = disp_width_;
            dci.ulTargetHeight = disp_height_;
            dci.ulNumDecodeSurfaces = kDecodeSurfaceCount;
            dci.ulNumOutputSurfaces = 1;
            dci.ulCreationFlags = cudaVideoCreate_PreferCUVID;
            dci.vidLock = ctx_lock_;

            if (cuvid_->cuvidCreateDecoder(&decoder_, &dci) != 0
                || !decoder_) {
                std::fprintf(stderr,
                    "unio-pipe: nvdec cuvidCreateDecoder failed "
                    "(%ux%u coded, %ux%u display)\n",
                    width_, height_, disp_width_, disp_height_);
                return 0;  // 0 = abort decode chain
            }
            std::fprintf(stderr,
                "unio-pipe: nvdec decoder ready %ux%u coded, "
                "%ux%u display, NV12\n",
                width_, height_, disp_width_, disp_height_);

            // Size the CPU NV12 ring. NV12 = Y plane (w*h) +
            // interleaved UV (w*h/2) = w*h*3/2 bytes. Using
            // disp-sized buffers since that's what the presenter
            // wants to paint. Stride aligned to 256 to match
            // typical GL upload alignment.
            const std::uint32_t stride_y =
                (disp_width_ + 255) & ~255u;
            const std::uint32_t stride_uv = stride_y;
            const std::size_t slot_bytes =
                static_cast<std::size_t>(stride_y) * disp_height_
                + static_cast<std::size_t>(stride_uv)
                  * (disp_height_ / 2);
            cpu_ring_stride_y_ = stride_y;
            cpu_ring_stride_uv_ = stride_uv;
            for (int i = 0; i < kCpuRingSlots; ++i) {
                cpu_ring_[i].resize(slot_bytes);
            }
        }
        // Returning the number of decode surfaces means "continue"
        // with that many — CUVID allocates internally.
        return kDecodeSurfaceCount;
    }

    int HandleDecode(CUVIDPICPARAMS* pic) {
        if (!decoder_) return 0;
        return cuvid_->cuvidDecodePicture(decoder_, pic) == 0 ? 1 : 0;
    }

    int HandleDisplay(CUVIDPARSERDISPINFO* info) {
        if (!decoder_ || !on_frame_) return 1;

        CUVIDPROCPARAMS proc{};
        proc.progressive_frame = info->progressive_frame;
        proc.second_field = info->repeat_first_field + 1;
        proc.top_field_first = info->top_field_first;
        proc.unpaired_field = (info->repeat_first_field < 0);

        CUdeviceptr dev_frame = 0;
        unsigned int dev_pitch = 0;
        if (cuvid_->cuvidMapVideoFrame(decoder_, info->picture_index,
                                          &dev_frame, &dev_pitch,
                                          &proc) != 0) {
            std::fprintf(stderr,
                "unio-pipe: nvdec cuvidMapVideoFrame "
                "failed pic=%d\n", info->picture_index);
            return 0;
        }

        // Push the shared CUcontext current on this thread — CUVID
        // calls the display callback from its internal worker;
        // cuMemcpy2D needs a current context.
        cuda_->cuCtxPushCurrent(cu_ctx_);

        const int slot = cpu_ring_head_++ % kCpuRingSlots;
        std::vector<std::uint8_t>& buf = cpu_ring_[slot];

        // NV12 on the GPU is one allocation: Y plane at
        // dev_frame[0 .. dev_pitch * height), followed immediately
        // by UV plane at dev_frame[dev_pitch * height .. +
        // dev_pitch * (height/2)). cuMemcpy2D copies each plane
        // into our contiguous CPU buffer at our stride.
        CUDA_MEMCPY2D m{};
        m.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        m.srcDevice = dev_frame;
        m.srcPitch = dev_pitch;
        m.dstMemoryType = CU_MEMORYTYPE_HOST;
        m.dstHost = buf.data();
        m.dstPitch = cpu_ring_stride_y_;
        m.WidthInBytes = disp_width_;
        m.Height = disp_height_;
        if (cuda_->cuMemcpy2D(&m) != 0) {
            cuvid_->cuvidUnmapVideoFrame(decoder_, dev_frame);
            CUcontext popped = nullptr;
            cuda_->cuCtxPopCurrent(&popped);
            return 0;
        }
        // UV plane
        m.srcDevice = dev_frame + static_cast<CUdeviceptr>(
            dev_pitch) * disp_height_;
        m.dstHost = buf.data()
                    + static_cast<std::size_t>(cpu_ring_stride_y_)
                      * disp_height_;
        m.dstPitch = cpu_ring_stride_uv_;
        m.Height = disp_height_ / 2;
        if (cuda_->cuMemcpy2D(&m) != 0) {
            cuvid_->cuvidUnmapVideoFrame(decoder_, dev_frame);
            CUcontext popped = nullptr;
            cuda_->cuCtxPopCurrent(&popped);
            return 0;
        }

        cuvid_->cuvidUnmapVideoFrame(decoder_, dev_frame);
        CUcontext popped = nullptr;
        cuda_->cuCtxPopCurrent(&popped);

        DecodedFrame df;
        df.source_kind = DecodedSurfaceKind::CpuNv12;
        df.cpu_nv12_y_ptr = buf.data();
        df.cpu_nv12_uv_ptr = buf.data()
            + static_cast<std::size_t>(cpu_ring_stride_y_)
              * disp_height_;
        df.cpu_nv12_stride_y = cpu_ring_stride_y_;
        df.cpu_nv12_stride_uv = cpu_ring_stride_uv_;
        df.width = disp_width_;
        df.height = disp_height_;
        df.decode_done_monotonic_ns = NowNs();
        df.frame_id = pending_frame_id_;
        df.capture_monotonic_ns = pending_capture_ns_;
        // CUVID display callbacks don't distinguish IDR vs P in
        // CUVIDPARSERDISPINFO — our own Annex-B scan on Feed()
        // already tracked the latest IDR arrival, so we just
        // pass a conservative "not keyframe" here. (Presenter +
        // status counter use it for UI paint, not for
        // correctness.)
        df.key_frame = false;
        ++frame_count_;
        on_frame_(df);
        pending_frame_id_ = 0;
        pending_capture_ns_ = 0;
        return 1;
    }

    void Teardown() {
        if (!cuvid_) return;
        if (parser_) {
            cuvid_->cuvidDestroyVideoParser(parser_);
            parser_ = nullptr;
        }
        if (decoder_) {
            cuvid_->cuvidDestroyDecoder(decoder_);
            decoder_ = nullptr;
        }
        if (ctx_lock_) {
            cuvid_->cuvidCtxLockDestroy(ctx_lock_);
            ctx_lock_ = nullptr;
        }
        // cuvid_free_functions releases the handle; we don't
        // own cuda_ / cu_ctx_ (CudaRuntime does).
        cuvid_free_functions(&cuvid_);
        cuvid_ = nullptr;
    }

    // ── State ───────────────────────────────────────────────────

    Config cfg_{};
    FrameReady on_frame_;

    // Shared runtimes. Not owned.
    CudaFunctions* cuda_ = nullptr;
    CUcontext cu_ctx_ = nullptr;

    // CUVID runtime + instance handles. Owned.
    CuvidFunctions* cuvid_ = nullptr;
    CUvideoctxlock ctx_lock_ = nullptr;
    CUvideoparser parser_ = nullptr;
    CUvideodecoder decoder_ = nullptr;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t disp_width_ = 0;
    std::uint32_t disp_height_ = 0;
    std::uint32_t cpu_ring_stride_y_ = 0;
    std::uint32_t cpu_ring_stride_uv_ = 0;
    std::vector<std::uint8_t> cpu_ring_[kCpuRingSlots];
    std::uint64_t cpu_ring_head_ = 0;
    std::uint64_t frame_count_ = 0;
    std::uint64_t pending_frame_id_ = 0;
    std::uint64_t pending_capture_ns_ = 0;
};

std::unique_ptr<Decoder> MakeNvdecDecoder() {
    // Cheap up-front: if CUDA or CUVID aren't loadable, decline so
    // the runtime probe / factory chain picks VA-API instead of
    // constructing a decoder that would fail on first Feed.
    if (!CudaRuntime::Instance().ready()) {
        std::fprintf(stderr,
            "unio-pipe: NVDEC factory declining — %s\n",
            CudaRuntime::Instance().reason().c_str());
        return nullptr;
    }
    return std::make_unique<NvdecDecoder>();
}

}  // namespace unio

#endif  // __linux__
