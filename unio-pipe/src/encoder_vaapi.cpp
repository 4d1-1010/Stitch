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

// Minimal H.264 bitstream writer. Only what we need to synthesize
// SPS + PPS + insert Annex-B start codes and emulation-prevention
// bytes. All H.264 headers are bit-packed MSB-first into a byte
// stream, with 00 00 03 inserted whenever three 00 bytes would
// otherwise appear consecutively in the RBSP (emulation
// prevention; the decoder strips them back out).
class BitWriter {
public:
    void WriteBits(std::uint32_t value, int nbits) {
        for (int i = nbits - 1; i >= 0; --i) {
            bit_buf_ = (bit_buf_ << 1) | ((value >> i) & 1);
            bit_count_++;
            if (bit_count_ == 8) {
                FlushByte();
            }
        }
    }

    // Exp-Golomb unsigned: prefix of floor(log2(N+1)) zeros,
    // then a 1 bit, then (N+1) in that many +1 bits.
    void WriteUe(std::uint32_t n) {
        std::uint32_t m = n + 1;
        int prefix = 0;
        while ((1u << (prefix + 1)) <= m) ++prefix;
        WriteBits(0, prefix);
        WriteBits(m, prefix + 1);
    }

    void WriteSe(std::int32_t n) {
        std::uint32_t u = (n <= 0)
            ? static_cast<std::uint32_t>(-2 * n)
            : static_cast<std::uint32_t>(2 * n - 1);
        WriteUe(u);
    }

    // RBSP trailing bits: a single 1 bit, then pad to byte boundary
    // with zeroes.
    void WriteRbspTrailing() {
        WriteBits(1, 1);
        while (bit_count_ != 0) WriteBits(0, 1);
    }

    // Emit an Annex-B NAL unit: start code, nal header byte, then
    // the accumulated RBSP bytes with emulation prevention.
    std::vector<std::uint8_t> EmitAnnexB(std::uint8_t nal_header) const {
        std::vector<std::uint8_t> out;
        out.reserve(bytes_.size() + 8);
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.push_back(nal_header);
        int zero_run = 0;
        for (auto b : bytes_) {
            if (zero_run >= 2 && b <= 0x03) {
                out.push_back(0x03);
                zero_run = 0;
            }
            out.push_back(b);
            zero_run = (b == 0) ? (zero_run + 1) : 0;
        }
        return out;
    }

private:
    void FlushByte() {
        bytes_.push_back(bit_buf_ & 0xFF);
        bit_buf_ = 0;
        bit_count_ = 0;
    }

    std::vector<std::uint8_t> bytes_;
    std::uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
};

// H.264 Sequence Parameter Set RBSP (Main profile, no VUI, no
// scaling matrix). The values here must match every field we set
// in the VAEncSequenceParameterBufferH264 above or the decoder
// will reject the slice with "non-existing SPS / PPS referenced".
std::vector<std::uint8_t> BuildSpsNal(int profile_idc,
                                      int level_idc,
                                      int mbs_w, int mbs_h,
                                      int px_w, int px_h) {
    BitWriter w;
    w.WriteBits(profile_idc, 8);       // profile_idc
    w.WriteBits(0, 8);                  // constraint_set*_flag + zero bits
    w.WriteBits(level_idc, 8);          // level_idc
    w.WriteUe(0);                       // seq_parameter_set_id
    w.WriteUe(1);                       // chroma_format_idc = 4:2:0
    w.WriteUe(0);                       // bit_depth_luma_minus8
    w.WriteUe(0);                       // bit_depth_chroma_minus8
    w.WriteBits(0, 1);                  // qpprime_y_zero_transform_bypass
    w.WriteBits(0, 1);                  // seq_scaling_matrix_present
    w.WriteUe(0);                       // log2_max_frame_num_minus4
    w.WriteUe(2);                       // pic_order_cnt_type = 2
    w.WriteUe(2);                       // num_ref_frames
    w.WriteBits(0, 1);                  // gaps_in_frame_num_allowed
    w.WriteUe(mbs_w - 1);               // pic_width_in_mbs_minus1
    w.WriteUe(mbs_h - 1);               // pic_height_in_map_units_minus1
    w.WriteBits(1, 1);                  // frame_mbs_only_flag
    w.WriteBits(1, 1);                  // direct_8x8_inference_flag
    const bool crop = (px_w & 15) || (px_h & 15);
    w.WriteBits(crop ? 1 : 0, 1);       // frame_cropping_flag
    if (crop) {
        w.WriteUe(0);
        w.WriteUe((mbs_w * 16 - px_w) / 2);
        w.WriteUe(0);
        w.WriteUe((mbs_h * 16 - px_h) / 2);
    }
    w.WriteBits(0, 1);                  // vui_parameters_present_flag
    w.WriteRbspTrailing();
    return w.EmitAnnexB(0x67);          // nal_ref_idc=3, nal_type=7 (SPS)
}

// H.264 Picture Parameter Set RBSP. entropy_coding_mode_flag must
// agree with what we asked the driver to emit in the slice — pass
// it through so Main/High (CABAC) and ConstrainedBaseline (CAVLC)
// can both be built from the same helper.
std::vector<std::uint8_t> BuildPpsNal(bool cabac, int pic_init_qp) {
    BitWriter w;
    w.WriteUe(0);                       // pic_parameter_set_id
    w.WriteUe(0);                       // seq_parameter_set_id
    w.WriteBits(cabac ? 1 : 0, 1);      // entropy_coding_mode_flag
    w.WriteBits(0, 1);                  // bottom_field_pic_order_in_frame_present
    w.WriteUe(0);                       // num_slice_groups_minus1
    w.WriteUe(0);                       // num_ref_idx_l0_default_active_minus1
    w.WriteUe(0);                       // num_ref_idx_l1_default_active_minus1
    w.WriteBits(0, 1);                  // weighted_pred_flag
    w.WriteBits(0, 2);                  // weighted_bipred_idc
    w.WriteSe(pic_init_qp - 26);        // pic_init_qp_minus26
    w.WriteSe(0);                       // pic_init_qs_minus26
    w.WriteSe(0);                       // chroma_qp_index_offset
    w.WriteBits(1, 1);                  // deblocking_filter_control_present
    w.WriteBits(0, 1);                  // constrained_intra_pred_flag
    w.WriteBits(0, 1);                  // redundant_pic_cnt_present_flag
    w.WriteRbspTrailing();
    return w.EmitAnnexB(0x68);          // nal_ref_idc=3, nal_type=8 (PPS)
}

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

        // Try profiles in preference order. Intel iHD and most AMD
        // drivers only advertise Main / High; NVIDIA's VA-API
        // driver adds ConstrainedBaseline. We don't actually need
        // baseline specifically — we enforce IPPP + 1 reference
        // frame in the picture params, which is a strict subset of
        // every H.264 profile.
        //
        // Entry point: EncSlice is the full encoder path, EncSliceLP
        // the low-power path. Intel Gen9+ iGPUs expose only the LP
        // path; NVIDIA's driver exposes EncSlice. Accept either —
        // the API shape is identical and the user-facing quality
        // difference is small enough to not block ship.
        const VAProfile candidates[] = {
            VAProfileH264Main,
            VAProfileH264ConstrainedBaseline,
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

        VAConfigAttrib attrs[2];
        attrs[0].type = VAConfigAttribRTFormat;
        attrs[1].type = VAConfigAttribRateControl;
        s = vaGetConfigAttributes(
            dpy_, profile_, entrypoint_, attrs, 2);
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

        VAConfigAttrib cfg_attrs[2];
        cfg_attrs[0].type = VAConfigAttribRTFormat;
        cfg_attrs[0].value = VA_RT_FORMAT_YUV420;
        cfg_attrs[1].type = VAConfigAttribRateControl;
        cfg_attrs[1].value = VA_RC_CQP;
        s = vaCreateConfig(
            dpy_, profile_, entrypoint_,
            cfg_attrs, 2, &config_id_);
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
                     "BGRX→NV12 upload via vaDeriveImage, CQP %d\n",
                     major, minor, profile_name, ep_name,
                     cfg.width, cfg.height, cfg.quality);

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
        param_bufs.reserve(5);

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

        // Intel SliceLP (and some other drivers) do NOT embed SPS/
        // PPS in the coded bitstream. Prepend our own every IDR so
        // downstream decoders always see a self-contained GOP.
        if (is_idr) {
            const int profile_idc =
                profile_ == VAProfileH264ConstrainedBaseline ? 66
                : profile_ == VAProfileH264High ? 100
                : 77;  // Main
            const bool cabac =
                profile_ != VAProfileH264ConstrainedBaseline;
            auto sps = BuildSpsNal(profile_idc, 40, mbs_w_, mbs_h_,
                                    cfg_.width, cfg_.height);
            auto pps = BuildPpsNal(cabac, cfg_.quality);
            pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                                  sps.begin(), sps.end());
            pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                                  pps.begin(), pps.end());
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
