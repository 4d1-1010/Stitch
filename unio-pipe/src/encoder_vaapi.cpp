// VA-API H.264 encoder (Day 4 — real encode).
//
//   BGRX host buffer  ──► VAImage(BGRX) ──► vaPutImage ──► NV12 surface
//                                                          │
//                                  VAEncSequenceParam      │
//                                  VAEncPictureParam  ─────┤ vaRenderPicture
//                                  VAEncSliceParam         │
//                                                          ▼
//                                                  vaEndPicture
//                                                  vaSyncSurface
//                                                  vaMapBuffer(coded) ──► Annex-B NALs
//
// IPPP… GOP — constrained baseline means no B-frames, and we keep
// exactly one reference surface. With three surfaces in the pool,
// frame N's reference (N-1) is always a different slot than N's
// reconstruction target so nothing overwrites an in-flight ref.
//
// SPS / PPS emission is left to the driver: Intel and the NVIDIA
// VA-API driver both embed headers in the coded buffer at IDR when
// no VA_ENC_PACKED_HEADER_SEQUENCE is requested. That matches how
// the Python pipeline already produces bitstream.

#if !defined(__linux__) || !defined(UNIO_PIPE_HAS_VAAPI)
#include "encoder.h"
namespace unio {
std::unique_ptr<Encoder> MakeVaapiEncoder() { return nullptr; }
}  // namespace unio
#else

#include "encoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count());
}

// Macroblock count ceiling-divided. H.264 always codes in 16×16 MB;
// sources that aren't MB-aligned get cropped via SPS frame_crop_*.
constexpr int MbsCeil(int px) { return (px + 15) / 16; }

class VaapiEncoder final : public Encoder {
public:
    VaapiEncoder() = default;

    ~VaapiEncoder() override {
        Teardown();
    }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;
        mbs_w_ = MbsCeil(cfg.width);
        mbs_h_ = MbsCeil(cfg.height);

        drm_fd_ = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm_fd_ < 0) {
            return "cannot open /dev/dri/renderD128 (install "
                   "vainfo + ensure user is in 'render' group)";
        }

        dpy_ = vaGetDisplayDRM(drm_fd_);
        if (!dpy_) {
            ::close(drm_fd_);
            drm_fd_ = -1;
            return "vaGetDisplayDRM failed";
        }
        int major = 0;
        int minor = 0;
        VAStatus s = vaInitialize(dpy_, &major, &minor);
        if (s != VA_STATUS_SUCCESS) {
            Teardown();
            return std::string("vaInitialize: ") + vaErrorStr(s);
        }

        // Try profiles in preference order. We pick
        // ConstrainedBaseline first because it's the lowest common
        // denominator — no CABAC, no 8×8 transform, no B-frames —
        // which means the slice bitstream that every driver emits
        // is parseable by every decoder with the fewest
        // vendor-specific bits. Main and High are accepted only
        // when a driver refuses CB (some AMD AMF-via-VA-API builds
        // do); they bring CABAC in, which has more driver quirks.
        //
        // Entry point: EncSlice is the full encoder path, EncSliceLP
        // the low-power path. Intel Gen9+ iGPUs expose only the LP
        // path; NVIDIA's driver exposes EncSlice. Accept either —
        // the API shape is identical.
        const VAProfile candidates[] = {
            VAProfileH264ConstrainedBaseline,
            VAProfileH264Main,
            VAProfileH264High,
        };
        profile_ = VAProfileNone;
        entrypoint_ = VAEntrypointEncSlice;
        for (VAProfile p : candidates) {
            int num_epoints = 0;
            VAEntrypoint epoints[16];
            VAStatus qs = vaQueryConfigEntrypoints(
                dpy_, p, epoints, &num_epoints);
            if (qs != VA_STATUS_SUCCESS) continue;
            bool have_slice = false, have_slice_lp = false;
            for (int i = 0; i < num_epoints; ++i) {
                if (epoints[i] == VAEntrypointEncSlice) have_slice = true;
                if (epoints[i] == VAEntrypointEncSliceLP) have_slice_lp = true;
            }
            if (have_slice) {
                profile_ = p;
                entrypoint_ = VAEntrypointEncSlice;
                break;
            }
            if (have_slice_lp && profile_ == VAProfileNone) {
                profile_ = p;
                entrypoint_ = VAEntrypointEncSliceLP;
                // Keep looking — a later profile might still have
                // full EncSlice, which we'd prefer.
            }
        }
        if (profile_ == VAProfileNone) {
            Teardown();
            return "VA-API driver does not advertise H.264 "
                   "Slice / SliceLP encode for Main / "
                   "ConstrainedBaseline / High";
        }

        VAConfigAttrib attrs[3];
        attrs[0].type = VAConfigAttribRTFormat;
        attrs[1].type = VAConfigAttribRateControl;
        attrs[2].type = VAConfigAttribEncPackedHeaders;
        s = vaGetConfigAttributes(
            dpy_, profile_, entrypoint_, attrs, 3);
        if (s != VA_STATUS_SUCCESS) {
            Teardown();
            return std::string("vaGetConfigAttributes: ")
                   + vaErrorStr(s);
        }
        if (!(attrs[0].value & VA_RT_FORMAT_YUV420)) {
            Teardown();
            return "driver does not support YUV420 encode format";
        }
        if (!(attrs[1].value & VA_RC_CQP)) {
            Teardown();
            return "driver lacks CQP rate control (CBR-only "
                   "path not wired yet)";
        }

        // Packed headers let us supply SPS / PPS / slice-header
        // bit-for-bit instead of relying on the driver to emit
        // them. Every H.264 VA-API driver worth using (Intel iHD,
        // NVIDIA nvidia-vaapi-driver, AMD Mesa radeonsi) advertises
        // at least SEQUENCE+PICTURE; most add SLICE. When SLICE is
        // there we take full control of the slice header, which is
        // the only way to get one binary that decodes on every
        // vendor — driver-written slice headers differ in
        // num_ref_idx_active_override_flag handling, dec_ref_pic
        // marking, and deblocking flag ordering in ways that ffmpeg
        // rejects if your SPS/PPS don't match exactly.
        const std::uint32_t pkt = attrs[2].value == VA_ATTRIB_NOT_SUPPORTED
                                      ? 0u
                                      : attrs[2].value;
        packed_seq_ = (pkt & VA_ENC_PACKED_HEADER_SEQUENCE) != 0;
        packed_pic_ = (pkt & VA_ENC_PACKED_HEADER_PICTURE) != 0;
        packed_slice_ = (pkt & VA_ENC_PACKED_HEADER_SLICE) != 0;

        VAConfigAttrib cfg_attrs[3];
        int n_cfg_attrs = 2;
        cfg_attrs[0].type = VAConfigAttribRTFormat;
        cfg_attrs[0].value = VA_RT_FORMAT_YUV420;
        cfg_attrs[1].type = VAConfigAttribRateControl;
        cfg_attrs[1].value = VA_RC_CQP;
        std::uint32_t packed_request = 0;
        if (packed_seq_) packed_request |= VA_ENC_PACKED_HEADER_SEQUENCE;
        if (packed_pic_) packed_request |= VA_ENC_PACKED_HEADER_PICTURE;
        if (packed_slice_) packed_request |= VA_ENC_PACKED_HEADER_SLICE;
        if (packed_request != 0) {
            cfg_attrs[2].type = VAConfigAttribEncPackedHeaders;
            cfg_attrs[2].value = packed_request;
            n_cfg_attrs = 3;
        }
        s = vaCreateConfig(
            dpy_, profile_, entrypoint_,
            cfg_attrs, n_cfg_attrs, &config_id_);
        if (s != VA_STATUS_SUCCESS) {
            Teardown();
            return std::string("vaCreateConfig: ") + vaErrorStr(s);
        }

        std::uint32_t w = static_cast<std::uint32_t>(cfg.width);
        std::uint32_t h = static_cast<std::uint32_t>(cfg.height);
        VASurfaceAttrib surface_attr;
        surface_attr.type = VASurfaceAttribPixelFormat;
        surface_attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
        surface_attr.value.type = VAGenericValueTypeInteger;
        surface_attr.value.value.i = VA_FOURCC_NV12;
        s = vaCreateSurfaces(
            dpy_, VA_RT_FORMAT_YUV420, w, h, surfaces_,
            kNumSurfaces, &surface_attr, 1);
        if (s != VA_STATUS_SUCCESS) {
            Teardown();
            return std::string("vaCreateSurfaces: ")
                   + vaErrorStr(s);
        }

        s = vaCreateContext(
            dpy_, config_id_, static_cast<int>(w),
            static_cast<int>(h), VA_PROGRESSIVE, surfaces_,
            kNumSurfaces, &context_id_);
        if (s != VA_STATUS_SUCCESS) {
            Teardown();
            return std::string("vaCreateContext: ")
                   + vaErrorStr(s);
        }

        if (auto err = SetupUploadImage(); err) {
            Teardown();
            return err;
        }

        const char* profile_name =
            profile_ == VAProfileH264Main ? "Main"
            : profile_ == VAProfileH264High ? "High"
            : profile_ == VAProfileH264ConstrainedBaseline
                 ? "ConstrainedBaseline" : "?";
        const char* ep_name =
            entrypoint_ == VAEntrypointEncSlice ? "Slice" : "SliceLP";
        std::fprintf(stderr,
                     "unio-pipe: VA-API initialized %d.%d, "
                     "profile %s (%s), surfaces %dx%d NV12, "
                     "packed headers %s%s%s, CQP %d\n",
                     major, minor, profile_name, ep_name,
                     cfg.width, cfg.height,
                     packed_seq_   ? "SEQ "   : "",
                     packed_pic_   ? "PIC "   : "",
                     packed_slice_ ? "SLICE " : "",
                     cfg.quality);

        initialized_ = true;
        return std::nullopt;
    }

    void ForceIdr() override {
        force_idr_.store(true, std::memory_order_release);
    }

    EncodedPacketPtr Encode(const CpuFrame& frame) override {
        if (!initialized_) return nullptr;

        auto log_once = [&](const char* where, VAStatus st) {
            if (first_error_logged_) return;
            first_error_logged_ = true;
            std::fprintf(stderr,
                         "unio-pipe: encode failed at %s: %s\n",
                         where, vaErrorStr(st));
        };

        const bool is_idr =
            force_idr_.exchange(false, std::memory_order_acq_rel)
            || frame_count_ == 0;
        if (is_idr) {
            frame_num_ = 0;
            idr_pic_id_++;
        }

        const int src_idx = frame_count_ % kNumSurfaces;
        const VASurfaceID curr_surface = surfaces_[src_idx];

        if (!UploadBgrx(curr_surface, frame)) {
            log_once("UploadBgrx", VA_STATUS_ERROR_OPERATION_FAILED);
            return nullptr;
        }

        VABufferID coded_buf = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(
            dpy_, context_id_, VAEncCodedBufferType,
            coded_buffer_bytes_, 1, nullptr, &coded_buf);
        if (s != VA_STATUS_SUCCESS) {
            log_once("coded vaCreateBuffer", s);
            return nullptr;
        }

        std::vector<VABufferID> param_bufs;
        param_bufs.reserve(12);

        // Misc rate control — CQP uses initial_qp as the constant.
        if (VABufferID b = BuildRateControl(); b != VA_INVALID_ID) {
            param_bufs.push_back(b);
        } else {
            vaDestroyBuffer(dpy_, coded_buf);
            return nullptr;
        }

        if (is_idr) {
            if (VABufferID b = BuildSequenceParam();
                b != VA_INVALID_ID) {
                param_bufs.push_back(b);
            } else {
                DestroyAll(param_bufs);
                vaDestroyBuffer(dpy_, coded_buf);
                return nullptr;
            }
        }

        VABufferID pic_buf =
            BuildPictureParam(curr_surface, coded_buf, is_idr);
        if (pic_buf == VA_INVALID_ID) {
            DestroyAll(param_bufs);
            vaDestroyBuffer(dpy_, coded_buf);
            return nullptr;
        }
        param_bufs.push_back(pic_buf);

        // Packed SPS + PPS go in on every IDR. Packing order
        // doesn't matter to the encoder, but it's clean to emit
        // them before the slice so the coded buffer comes out
        // Annex-B-ordered [SPS][PPS][slice] as decoders expect.
        if (is_idr && packed_seq_) {
            const int profile_idc =
                profile_ == VAProfileH264ConstrainedBaseline ? 66
                : profile_ == VAProfileH264High ? 100 : 77;
            auto sps = BuildSps(profile_idc, 40, mbs_w_, mbs_h_,
                                cfg_.width, cfg_.height);
            if (!SubmitPackedHeader(VAEncPackedHeaderSequence,
                                    sps, param_bufs)) {
                DestroyAll(param_bufs);
                vaDestroyBuffer(dpy_, coded_buf);
                return nullptr;
            }
        }
        if (is_idr && packed_pic_) {
            const bool cabac =
                profile_ != VAProfileH264ConstrainedBaseline;
            auto pps = BuildPps(cabac, cfg_.quality);
            if (!SubmitPackedHeader(VAEncPackedHeaderPicture,
                                    pps, param_bufs)) {
                DestroyAll(param_bufs);
                vaDestroyBuffer(dpy_, coded_buf);
                return nullptr;
            }
        }
        if (packed_slice_) {
            auto sh = BuildSliceHeader(is_idr, frame_num_,
                                       idr_pic_id_);
            if (!SubmitPackedHeader(VAEncPackedHeaderSlice,
                                    sh, param_bufs)) {
                DestroyAll(param_bufs);
                vaDestroyBuffer(dpy_, coded_buf);
                return nullptr;
            }
        }

        VABufferID slice_buf = BuildSliceParam(is_idr);
        if (slice_buf == VA_INVALID_ID) {
            DestroyAll(param_bufs);
            vaDestroyBuffer(dpy_, coded_buf);
            return nullptr;
        }
        param_bufs.push_back(slice_buf);

        s = vaBeginPicture(dpy_, context_id_, curr_surface);
        if (s != VA_STATUS_SUCCESS) {
            log_once("vaBeginPicture", s);
            DestroyAll(param_bufs);
            vaDestroyBuffer(dpy_, coded_buf);
            return nullptr;
        }
        s = vaRenderPicture(dpy_, context_id_,
                            param_bufs.data(),
                            static_cast<int>(param_bufs.size()));
        if (s != VA_STATUS_SUCCESS) {
            log_once("vaRenderPicture", s);
            vaEndPicture(dpy_, context_id_);
            DestroyAll(param_bufs);
            vaDestroyBuffer(dpy_, coded_buf);
            return nullptr;
        }
        s = vaEndPicture(dpy_, context_id_);
        if (s != VA_STATUS_SUCCESS) {
            log_once("vaEndPicture", s);
            DestroyAll(param_bufs);
            vaDestroyBuffer(dpy_, coded_buf);
            return nullptr;
        }
        // vaRenderPicture hands ownership of the param buffers to
        // the driver — they're consumed by vaEndPicture. Don't
        // destroy them again here.

        s = vaSyncSurface(dpy_, curr_surface);
        if (s != VA_STATUS_SUCCESS) {
            log_once("vaSyncSurface", s);
            vaDestroyBuffer(dpy_, coded_buf);
            return nullptr;
        }

        auto pkt = std::make_unique<EncodedPacket>();
        pkt->frame_id = frame.frame_id;
        pkt->capture_monotonic_ns = frame.capture_monotonic_ns;
        pkt->key_frame = is_idr;

        // With packed headers enabled the driver emits our SPS +
        // PPS + slice header verbatim in front of the slice data,
        // so the coded buffer already contains a self-contained
        // Annex-B frame. Fallback: if SEQ+PIC packing isn't
        // supported (very old drivers), prepend manually so the
        // sink still sees a decodable GOP. Intel iHD + NVIDIA +
        // Mesa radeonsi all advertise packed headers as of 2024.
        if (is_idr && !packed_seq_) {
            const int profile_idc =
                profile_ == VAProfileH264ConstrainedBaseline ? 66
                : profile_ == VAProfileH264High ? 100
                : 77;
            auto sps = BuildSps(profile_idc, 40, mbs_w_, mbs_h_,
                                 cfg_.width, cfg_.height);
            pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                                  sps.bytes.begin(),
                                  sps.bytes.end());
        }
        if (is_idr && !packed_pic_) {
            const bool cabac =
                profile_ != VAProfileH264ConstrainedBaseline;
            auto pps = BuildPps(cabac, cfg_.quality);
            pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                                  pps.bytes.begin(),
                                  pps.bytes.end());
        }

        VACodedBufferSegment* segs = nullptr;
        s = vaMapBuffer(dpy_, coded_buf,
                        reinterpret_cast<void**>(&segs));
        if (s != VA_STATUS_SUCCESS || segs == nullptr) {
            vaDestroyBuffer(dpy_, coded_buf);
            return nullptr;
        }
        for (auto* seg = segs; seg;
             seg = static_cast<VACodedBufferSegment*>(seg->next)) {
            if (seg->status & VA_CODED_BUF_STATUS_BAD_BITSTREAM) {
                vaUnmapBuffer(dpy_, coded_buf);
                vaDestroyBuffer(dpy_, coded_buf);
                return nullptr;
            }
            const auto* src =
                static_cast<const std::uint8_t*>(seg->buf);
            pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                                  src, src + seg->size);
        }
        vaUnmapBuffer(dpy_, coded_buf);
        vaDestroyBuffer(dpy_, coded_buf);

        // Rotate references: this surface becomes next frame's ref.
        prev_ref_surface_ = curr_surface;
        prev_frame_num_ = frame_num_;
        frame_num_ = (frame_num_ + 1) % kMaxFrameNum;
        frame_count_++;

        pkt->encode_done_monotonic_ns = NowNs();
        return pkt;
    }

    std::string_view Name() const override { return "vaapi"; }

private:
    static constexpr int kNumSurfaces = 3;
    static constexpr int kMaxFrameNum = 16;  // log2_max_frame_num_minus4=0

    std::optional<std::string> SetupUploadImage() {
        // Worst-case compressed frame — overestimate generously.
        // Live H.264 rarely exceeds 1 bit/pixel even at IDR; 3 bits
        // is a safe headroom that doesn't cost us anything since
        // the coded buffer is only allocated for the time between
        // vaRenderPicture and vaDestroyBuffer.
        coded_buffer_bytes_ =
            static_cast<std::uint32_t>(cfg_.width) * cfg_.height * 3 / 8
            + 4096;
        return std::nullopt;
    }

    // BGRX (host) → NV12 (surface). Uses vaDeriveImage so we write
    // straight into the surface backing store instead of staging
    // through a VAImage + vaPutImage — Intel iHD refuses
    // vaPutImage with a cross-fourcc target, but vaDeriveImage is
    // supported on every driver that ships VA-API 1.x.
    //
    // Colour conversion is BT.601 limited range on the CPU. At
    // 1080p that's ~5ms of memory-bandwidth-bound work per frame,
    // measurable but well under our per-frame budget. Moving it to
    // VPP (or a shader) is a PR 9 optimisation — we'd need a
    // second surface pool and a VAProcPipeline context.
    bool UploadBgrx(VASurfaceID surface, const CpuFrame& frame) {
        VAImage img{};
        VAStatus s = vaDeriveImage(dpy_, surface, &img);
        if (s != VA_STATUS_SUCCESS) {
            if (!first_error_logged_) {
                std::fprintf(stderr,
                    "unio-pipe: vaDeriveImage: %s\n", vaErrorStr(s));
            }
            return false;
        }
        if (img.format.fourcc != VA_FOURCC_NV12) {
            if (!first_error_logged_) {
                std::fprintf(stderr,
                    "unio-pipe: derived image is fourcc %c%c%c%c, "
                    "expected NV12\n",
                    (img.format.fourcc >> 0) & 0xFF,
                    (img.format.fourcc >> 8) & 0xFF,
                    (img.format.fourcc >> 16) & 0xFF,
                    (img.format.fourcc >> 24) & 0xFF);
            }
            vaDestroyImage(dpy_, img.image_id);
            return false;
        }

        void* mapped = nullptr;
        s = vaMapBuffer(dpy_, img.buf, &mapped);
        if (s != VA_STATUS_SUCCESS) {
            vaDestroyImage(dpy_, img.image_id);
            return false;
        }

        auto* base = static_cast<std::uint8_t*>(mapped);
        auto* y_plane = base + img.offsets[0];
        auto* uv_plane = base + img.offsets[1];
        const std::uint32_t y_stride = img.pitches[0];
        const std::uint32_t uv_stride = img.pitches[1];
        const int w = cfg_.width;
        const int h = cfg_.height;
        const std::uint32_t src_stride = frame.stride_bytes;
        const auto* src = frame.pixels.data();

        // Y plane: per-pixel BT.601.
        for (int y = 0; y < h; ++y) {
            const auto* row = src + static_cast<std::size_t>(y)
                                        * src_stride;
            auto* yrow = y_plane + static_cast<std::size_t>(y)
                                        * y_stride;
            for (int x = 0; x < w; ++x) {
                const int B = row[x * 4 + 0];
                const int G = row[x * 4 + 1];
                const int R = row[x * 4 + 2];
                const int Y = (66 * R + 129 * G + 25 * B + 128) >> 8;
                yrow[x] = static_cast<std::uint8_t>(Y + 16);
            }
        }
        // UV plane: 2x2 chroma subsample, interleaved U,V.
        for (int yy = 0; yy < h / 2; ++yy) {
            const auto* row0 = src
                + static_cast<std::size_t>(yy * 2) * src_stride;
            const auto* row1 = src
                + static_cast<std::size_t>(yy * 2 + 1) * src_stride;
            auto* uvrow = uv_plane
                + static_cast<std::size_t>(yy) * uv_stride;
            for (int xx = 0; xx < w / 2; ++xx) {
                const int i0 = xx * 8;       // 2 src pixels * 4 bytes
                const int i1 = xx * 8 + 4;
                const int B = (row0[i0 + 0] + row0[i1 + 0]
                             + row1[i0 + 0] + row1[i1 + 0]) >> 2;
                const int G = (row0[i0 + 1] + row0[i1 + 1]
                             + row1[i0 + 1] + row1[i1 + 1]) >> 2;
                const int R = (row0[i0 + 2] + row0[i1 + 2]
                             + row1[i0 + 2] + row1[i1 + 2]) >> 2;
                const int U = ((-38 * R - 74 * G + 112 * B + 128) >> 8)
                              + 128;
                const int V = ((112 * R - 94 * G - 18 * B + 128) >> 8)
                              + 128;
                uvrow[xx * 2 + 0] = static_cast<std::uint8_t>(U);
                uvrow[xx * 2 + 1] = static_cast<std::uint8_t>(V);
            }
        }

        vaUnmapBuffer(dpy_, img.buf);
        vaDestroyImage(dpy_, img.image_id);
        return true;
    }

    // Submit a packed header (SPS / PPS / slice). Pushes both the
    // parameter buffer and the data buffer onto `out` so the
    // caller vaRenderPictures them together with the other
    // params. Lifetime: vaEndPicture consumes the buffers, so we
    // don't vaDestroyBuffer on success.
    bool SubmitPackedHeader(VAEncPackedHeaderType type,
                            const PackedHeader& h,
                            std::vector<VABufferID>& out) {
        VAEncPackedHeaderParameterBuffer param{};
        param.type = type;
        param.bit_length = h.bit_length;
        param.has_emulation_bytes = 1;

        VABufferID p_buf = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(
            dpy_, context_id_,
            VAEncPackedHeaderParameterBufferType,
            sizeof(param), 1, &param, &p_buf);
        if (s != VA_STATUS_SUCCESS) {
            if (!first_error_logged_) {
                std::fprintf(stderr,
                    "unio-pipe: packed hdr param (%d): %s\n",
                    static_cast<int>(type), vaErrorStr(s));
            }
            return false;
        }
        VABufferID d_buf = VA_INVALID_ID;
        s = vaCreateBuffer(
            dpy_, context_id_,
            VAEncPackedHeaderDataBufferType,
            static_cast<unsigned int>(h.bytes.size()), 1,
            const_cast<std::uint8_t*>(h.bytes.data()), &d_buf);
        if (s != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(dpy_, p_buf);
            if (!first_error_logged_) {
                std::fprintf(stderr,
                    "unio-pipe: packed hdr data (%d): %s\n",
                    static_cast<int>(type), vaErrorStr(s));
            }
            return false;
        }
        out.push_back(p_buf);
        out.push_back(d_buf);
        return true;
    }

    VABufferID BuildRateControl() {
        VABufferID misc = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(
            dpy_, context_id_, VAEncMiscParameterBufferType,
            sizeof(VAEncMiscParameterBuffer)
                + sizeof(VAEncMiscParameterRateControl),
            1, nullptr, &misc);
        if (s != VA_STATUS_SUCCESS) return VA_INVALID_ID;
        VAEncMiscParameterBuffer* mp = nullptr;
        s = vaMapBuffer(dpy_, misc, reinterpret_cast<void**>(&mp));
        if (s != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(dpy_, misc);
            return VA_INVALID_ID;
        }
        mp->type = VAEncMiscParameterTypeRateControl;
        auto* rc = reinterpret_cast<VAEncMiscParameterRateControl*>(
            mp->data);
        std::memset(rc, 0, sizeof(*rc));
        rc->initial_qp = static_cast<std::uint32_t>(cfg_.quality);
        rc->min_qp = 0;
        rc->window_size = 1000;
        rc->rc_flags.bits.disable_frame_skip = 1;
        vaUnmapBuffer(dpy_, misc);
        return misc;
    }

    VABufferID BuildSequenceParam() {
        VAEncSequenceParameterBufferH264 seq{};
        seq.seq_parameter_set_id = 0;
        seq.level_idc = 40;
        seq.intra_period = 0;   // we control IDR explicitly
        seq.intra_idr_period = 0;
        seq.ip_period = 1;      // IPPP…, no B frames
        // Intel's SliceLP driver writes num_ref_idx_l0_active_minus1
        // as 1 in the slice header even when we signal 0 (single
        // reference), which then overflows a DPB sized for 1.
        // Giving the SPS headroom for 2 frames avoids the mismatch
        // without changing the actual reference semantics — we
        // still only use surfaces_[prev] as the sole reference.
        seq.max_num_ref_frames = 2;
        seq.picture_width_in_mbs =
            static_cast<std::uint16_t>(mbs_w_);
        seq.picture_height_in_mbs =
            static_cast<std::uint16_t>(mbs_h_);
        seq.bits_per_second = 0;
        seq.time_scale = 2 * static_cast<std::uint32_t>(cfg_.fps);
        seq.num_units_in_tick = 1;
        seq.seq_fields.bits.chroma_format_idc = 1;
        seq.seq_fields.bits.frame_mbs_only_flag = 1;
        seq.seq_fields.bits.pic_order_cnt_type = 2;
        seq.seq_fields.bits.log2_max_frame_num_minus4 = 0;
        seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
        seq.bit_depth_luma_minus8 = 0;
        seq.bit_depth_chroma_minus8 = 0;
        if ((cfg_.width & 15) || (cfg_.height & 15)) {
            seq.frame_cropping_flag = 1;
            seq.frame_crop_right_offset =
                (mbs_w_ * 16 - cfg_.width) / 2;
            seq.frame_crop_bottom_offset =
                (mbs_h_ * 16 - cfg_.height) / 2;
        }
        seq.vui_parameters_present_flag = 0;

        VABufferID buf = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(
            dpy_, context_id_,
            VAEncSequenceParameterBufferType,
            sizeof(seq), 1, &seq, &buf);
        return s == VA_STATUS_SUCCESS ? buf : VA_INVALID_ID;
    }

    VABufferID BuildPictureParam(VASurfaceID curr,
                                 VABufferID coded_buf,
                                 bool is_idr) {
        VAEncPictureParameterBufferH264 pic{};
        pic.CurrPic.picture_id = curr;
        pic.CurrPic.frame_idx = frame_num_;
        pic.CurrPic.flags = 0;
        pic.CurrPic.TopFieldOrderCnt = frame_num_ * 2;
        pic.CurrPic.BottomFieldOrderCnt = frame_num_ * 2;
        for (auto& rp : pic.ReferenceFrames) {
            rp.picture_id = VA_INVALID_SURFACE;
            rp.frame_idx = 0;
            rp.flags = VA_PICTURE_H264_INVALID;
            rp.TopFieldOrderCnt = 0;
            rp.BottomFieldOrderCnt = 0;
        }
        if (!is_idr && prev_ref_surface_ != VA_INVALID_SURFACE) {
            auto& r = pic.ReferenceFrames[0];
            r.picture_id = prev_ref_surface_;
            r.frame_idx = prev_frame_num_;
            r.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            r.TopFieldOrderCnt = prev_frame_num_ * 2;
            r.BottomFieldOrderCnt = prev_frame_num_ * 2;
        }
        pic.coded_buf = coded_buf;
        pic.pic_parameter_set_id = 0;
        pic.seq_parameter_set_id = 0;
        pic.frame_num = static_cast<std::uint16_t>(frame_num_);
        pic.pic_init_qp = static_cast<std::uint8_t>(cfg_.quality);
        pic.num_ref_idx_l0_active_minus1 = 0;
        pic.num_ref_idx_l1_active_minus1 = 0;
        pic.pic_fields.bits.idr_pic_flag = is_idr ? 1 : 0;
        pic.pic_fields.bits.reference_pic_flag = 1;
        // Main/High profiles want CABAC; ConstrainedBaseline bans
        // it. Intel SliceLP in particular rejects CAVLC on Main, so
        // this flag has to track the chosen profile.
        pic.pic_fields.bits.entropy_coding_mode_flag =
            (profile_ == VAProfileH264ConstrainedBaseline) ? 0 : 1;
        pic.pic_fields.bits.transform_8x8_mode_flag = 0;
        pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;
        pic.last_picture = 0;

        VABufferID buf = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(
            dpy_, context_id_,
            VAEncPictureParameterBufferType,
            sizeof(pic), 1, &pic, &buf);
        return s == VA_STATUS_SUCCESS ? buf : VA_INVALID_ID;
    }

    VABufferID BuildSliceParam(bool is_idr) {
        VAEncSliceParameterBufferH264 slice{};
        slice.macroblock_address = 0;
        slice.num_macroblocks =
            static_cast<std::uint32_t>(mbs_w_ * mbs_h_);
        // slice_type 7 = all-I (for IDR), 5 = all-P.
        slice.slice_type = is_idr ? 7 : 5;
        slice.pic_parameter_set_id = 0;
        slice.idr_pic_id = static_cast<std::uint16_t>(idr_pic_id_);
        slice.pic_order_cnt_lsb = 0;  // pic_order_cnt_type=2 → unused
        slice.direct_spatial_mv_pred_flag = 0;
        // Force the driver to write num_ref_idx_l0_active_minus1
        // into the slice header explicitly — Intel SliceLP ignores
        // the PPS default and decides its own number otherwise,
        // tripping ffmpeg's reference-count check.
        slice.num_ref_idx_active_override_flag = 1;
        slice.num_ref_idx_l0_active_minus1 = 0;
        slice.num_ref_idx_l1_active_minus1 = 0;
        for (auto& r : slice.RefPicList0) {
            r.picture_id = VA_INVALID_SURFACE;
            r.flags = VA_PICTURE_H264_INVALID;
        }
        for (auto& r : slice.RefPicList1) {
            r.picture_id = VA_INVALID_SURFACE;
            r.flags = VA_PICTURE_H264_INVALID;
        }
        if (!is_idr && prev_ref_surface_ != VA_INVALID_SURFACE) {
            auto& r = slice.RefPicList0[0];
            r.picture_id = prev_ref_surface_;
            r.frame_idx = prev_frame_num_;
            r.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            r.TopFieldOrderCnt = prev_frame_num_ * 2;
            r.BottomFieldOrderCnt = prev_frame_num_ * 2;
        }
        slice.luma_log2_weight_denom = 0;
        slice.chroma_log2_weight_denom = 0;
        slice.cabac_init_idc = 0;
        slice.slice_qp_delta = 0;
        slice.disable_deblocking_filter_idc = 0;
        slice.slice_alpha_c0_offset_div2 = 0;
        slice.slice_beta_offset_div2 = 0;

        VABufferID buf = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(
            dpy_, context_id_,
            VAEncSliceParameterBufferType,
            sizeof(slice), 1, &slice, &buf);
        return s == VA_STATUS_SUCCESS ? buf : VA_INVALID_ID;
    }

    void DestroyAll(std::vector<VABufferID>& bufs) {
        for (auto b : bufs) {
            if (b != VA_INVALID_ID) vaDestroyBuffer(dpy_, b);
        }
        bufs.clear();
    }

    void Teardown() {
        if (context_id_ != VA_INVALID_ID) {
            vaDestroyContext(dpy_, context_id_);
            context_id_ = VA_INVALID_ID;
        }
        if (surfaces_[0] != VA_INVALID_ID) {
            vaDestroySurfaces(dpy_, surfaces_, kNumSurfaces);
            for (auto& s : surfaces_) s = VA_INVALID_ID;
        }
        if (config_id_ != VA_INVALID_ID) {
            vaDestroyConfig(dpy_, config_id_);
            config_id_ = VA_INVALID_ID;
        }
        if (dpy_) {
            vaTerminate(dpy_);
            dpy_ = nullptr;
        }
        if (drm_fd_ >= 0) {
            ::close(drm_fd_);
            drm_fd_ = -1;
        }
        initialized_ = false;
    }

    Config cfg_{};
    int drm_fd_ = -1;
    VADisplay dpy_ = nullptr;
    VAProfile profile_ = VAProfileNone;
    VAEntrypoint entrypoint_ = VAEntrypointEncSlice;
    VAConfigID config_id_ = VA_INVALID_ID;
    VAContextID context_id_ = VA_INVALID_ID;
    VASurfaceID surfaces_[kNumSurfaces] = {
        VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    std::uint32_t coded_buffer_bytes_ = 0;
    int mbs_w_ = 0;
    int mbs_h_ = 0;
    bool initialized_ = false;
    bool packed_seq_ = false;
    bool packed_pic_ = false;
    bool packed_slice_ = false;

    // Per-frame state carried across Encode() calls.
    std::atomic<bool> force_idr_{true};
    std::uint64_t frame_count_ = 0;
    int frame_num_ = 0;
    int prev_frame_num_ = 0;
    int idr_pic_id_ = 0;
    VASurfaceID prev_ref_surface_ = VA_INVALID_SURFACE;
    bool first_error_logged_ = false;
};

}  // namespace

std::unique_ptr<Encoder> MakeVaapiEncoder() {
    return std::make_unique<VaapiEncoder>();
}

}  // namespace unio

#endif  // __linux__
