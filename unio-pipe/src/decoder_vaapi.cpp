// VA-API H.264 decoder. Mirror of encoder_vaapi.cpp on the sink
// side of the pipeline: Annex-B packets come in via QuicInbound;
// on each SPS/PPS we learn the picture size and build a VA decode
// context; each slice gets turned into a VAPictureParameterBufferH264
// + VASliceParameterBufferH264 + VASliceDataBufferType tuple that
// vaBeginPicture / vaRenderPicture / vaEndPicture consumes.
//
// Our streams are IPPP with a single reference, ConstrainedBaseline
// profile, CAVLC entropy, pic_order_cnt_type=2. That's a strict
// subset of what H.264 allows, so the DPB management is just:
// keep one short-term reference; after decoding each P, rotate.
//
// Cross-driver note: this decoder does the full H.264 bitstream
// parse itself rather than trusting any driver-side "auto-parse"
// behaviour. That keeps it portable across Intel iHD / NVIDIA
// vaapi-driver / Mesa radeonsi, and matches how Day 4b fixed the
// encoder for the same reason.

#if !defined(__linux__) || !defined(UNIO_PIPE_HAS_VAAPI)
#include "decoder.h"
namespace unio {
std::unique_ptr<Decoder> MakeVaapiDecoder() { return nullptr; }
}  // namespace unio
#else

#include "decoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

// H.264 decode structs (VAPictureParameterBufferH264 etc.) live
// in va.h itself on Ubuntu/Debian libva — unlike the encoder
// structs which are in va_enc_h264.h. No separate va_dec_h264.h.
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count());
}

class VaapiDecoder final : public Decoder {
public:
    VaapiDecoder() = default;
    ~VaapiDecoder() override {
        Teardown();
    }

    std::optional<std::string> Init(const Config& cfg,
                                    FrameReady on_frame) override {
        cfg_ = cfg;
        on_frame_ = std::move(on_frame);

        // Platform-native init: vaGetDisplayPlatform(VA_PLATFORM_DRM)
        // works on both X11 and Wayland without needing X11 at all.
        drm_fd_ = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm_fd_ < 0) {
            return "cannot open /dev/dri/renderD128";
        }
        dpy_ = vaGetDisplayDRM(drm_fd_);
        if (!dpy_) {
            ::close(drm_fd_);
            drm_fd_ = -1;
            Teardown();
            return "vaGetDisplayDRM failed";
        }
        int major = 0, minor = 0;
        if (VAStatus s = vaInitialize(dpy_, &major, &minor);
            s != VA_STATUS_SUCCESS) {
            Teardown();
            ::close(drm_fd_);
            drm_fd_ = -1;
            return std::string("vaInitialize: ") + vaErrorStr(s);
        }
        std::fprintf(stderr,
                     "unio-pipe: decoder VA-API %d.%d up\n",
                     major, minor);
        return std::nullopt;
    }

    std::optional<std::string> Feed(const std::uint8_t* bytes,
                                    std::size_t len) override {
        auto nals = ScanAnnexB(bytes, len);
        for (const auto& n : nals) {
            if (n.length == 0) continue;
            const std::uint8_t* nal = bytes + n.offset;
            const std::uint8_t hdr = nal[0];
            const std::uint8_t type = hdr & 0x1F;
            const std::uint8_t* nal_body = nal + 1;
            std::size_t body_len = n.length - 1;
            auto rbsp =
                StripEmulationPrevention(nal_body, body_len);
            // Debug log for the first N NALs, plus every SPS / PPS /
            // IDR forever, so we can see why fresh Windows-sourced
            // streams sometimes stall at frames_decoded=0.
            if (debug_nal_count_ < 30
                || type == kNalSps || type == kNalPps
                || type == kNalIdrSlice) {
                std::fprintf(stderr,
                    "unio-pipe: vaapi Feed NAL #%u type=%u len=%zu "
                    "rbsp=%zu have_sps=%d have_pps=%d\n",
                    debug_nal_count_,
                    static_cast<unsigned>(type),
                    n.length, rbsp.size(),
                    have_sps_ ? 1 : 0, have_pps_ ? 1 : 0);
                std::fflush(stderr);
            }
            ++debug_nal_count_;
            switch (type) {
                case kNalSps: HandleSps(rbsp.data(), rbsp.size()); break;
                case kNalPps: HandlePps(rbsp.data(), rbsp.size()); break;
                case kNalSei: {
                    std::uint64_t fid = 0, cap_ns = 0;
                    if (ParseLatencySei(rbsp.data(), rbsp.size(),
                                        fid, cap_ns)) {
                        pending_frame_id_ = fid;
                        pending_capture_ns_ = cap_ns;
                    }
                    break;
                }
                case kNalIdrSlice:
                case kNalNonIdrSlice:
                    HandleSlice(nal, n.length, rbsp.data(),
                                rbsp.size(), type == kNalIdrSlice);
                    break;
                default:
                    break;
            }
        }
        return std::nullopt;
    }

    std::string_view Name() const override { return "vaapi-h264"; }

private:
    static constexpr int kSurfaceCount = 4;

    void HandleSps(const std::uint8_t* rbsp, std::size_t len) {
        ParsedSps s{};
        const bool ok = ParseSps(rbsp, len, s);
        std::fprintf(stderr,
            "unio-pipe: vaapi HandleSps ok=%d profile=%d level=%d "
            "w_mbs=%d h_mbs=%d frame_mbs_only=%d poc_type=%d "
            "num_ref=%d\n",
            ok ? 1 : 0, s.profile_idc, s.level_idc,
            s.pic_width_in_mbs, s.pic_height_in_mbs,
            s.frame_mbs_only_flag ? 1 : 0,
            s.pic_order_cnt_type, s.num_ref_frames);
        std::fflush(stderr);
        if (!ok) return;
        sps_ = s;
        have_sps_ = true;
        const int px_w = sps_.pic_width_in_mbs * 16
                         - 2 * sps_.crop_right_offset;
        const int px_h = sps_.pic_height_in_mbs * 16
                         - 2 * sps_.crop_bottom_offset;
        if (have_context_
            && (px_w != width_ || px_h != height_)) {
            DestroyContext();
        }
        width_ = px_w;
        height_ = px_h;
    }

    void HandlePps(const std::uint8_t* rbsp, std::size_t len) {
        ParsedPps p{};
        const bool ok = ParsePps(rbsp, len, p);
        std::fprintf(stderr,
            "unio-pipe: vaapi HandlePps ok=%d cabac=%d "
            "num_slice_groups=%d\n",
            ok ? 1 : 0, p.entropy_coding_mode_flag ? 1 : 0,
            p.num_slice_groups_minus1);
        std::fflush(stderr);
        if (!ok) return;
        pps_ = p;
        have_pps_ = true;
    }

    void HandleSlice(const std::uint8_t* nal_full,
                     std::size_t nal_full_len,
                     const std::uint8_t* rbsp, std::size_t rbsp_len,
                     bool is_idr) {
        static int slice_log_count = 0;
        const bool log_slice = slice_log_count < 5 || is_idr;
        if (log_slice) {
            std::fprintf(stderr,
                "unio-pipe: vaapi HandleSlice is_idr=%d "
                "have_sps=%d have_pps=%d have_ctx=%d\n",
                is_idr ? 1 : 0,
                have_sps_ ? 1 : 0, have_pps_ ? 1 : 0,
                have_context_ ? 1 : 0);
            std::fflush(stderr);
            ++slice_log_count;
        }
        if (!have_sps_ || !have_pps_) return;
        if (!have_context_) {
            if (!OpenContext()) return;
        }
        ParsedSliceHeader sh{};
        const bool sh_ok = ParseSliceHeader(rbsp, rbsp_len, is_idr,
                                             sps_, pps_, sh);
        if (log_slice) {
            std::fprintf(stderr,
                "unio-pipe: vaapi ParseSliceHeader ok=%d "
                "slice_type_raw=%d frame_num=%d\n",
                sh_ok ? 1 : 0, sh.slice_type_raw, sh.frame_num);
            std::fflush(stderr);
        }
        if (!sh_ok) return;
        if (is_idr || prev_ref_surface_ == VA_INVALID_SURFACE) {
            if (!is_idr) {
                skipped_nonidr_++;
                return;
            }
            frame_num_mod_ = 0;
        }

        const VASurfaceID curr_surface = surfaces_[surface_idx_];
        surface_idx_ = (surface_idx_ + 1) % kSurfaceCount;

        // Build buffers
        VABufferID pic_buf = BuildPictureParam(curr_surface,
                                                sh, is_idr);
        if (pic_buf == VA_INVALID_ID) return;
        VABufferID slice_buf = BuildSliceParam(sh, nal_full_len);
        if (slice_buf == VA_INVALID_ID) {
            vaDestroyBuffer(dpy_, pic_buf);
            return;
        }
        // VA-API wants the slice_data buffer to start with the
        // Annex-B start code (0x00 0x00 0x00 0x01) — our
        // ScanAnnexB strips it before calling us, so put it
        // back. Without this, Intel iHD silently leaves the
        // output surface at its fill value (128 / gray) because
        // it can't find a valid NAL boundary. All the VA_STATUS
        // codes come back SUCCESS so the bug is invisible at
        // our level.
        slice_scratch_.clear();
        slice_scratch_.reserve(4 + nal_full_len);
        slice_scratch_.insert(slice_scratch_.end(),
            {0x00, 0x00, 0x00, 0x01});
        slice_scratch_.insert(slice_scratch_.end(),
            nal_full, nal_full + nal_full_len);

        VABufferID data_buf = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(
            dpy_, context_, VASliceDataBufferType,
            static_cast<unsigned int>(slice_scratch_.size()), 1,
            slice_scratch_.data(), &data_buf);
        if (s != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(dpy_, pic_buf);
            vaDestroyBuffer(dpy_, slice_buf);
            return;
        }

        // Intel iHD requires an IQMatrix buffer even when no
        // scaling lists are present. Fill with all-16 (ITU
        // default "no scaling") and submit alongside picture +
        // slice params. Without this the driver returns SUCCESS
        // on every call but produces a fill-value output
        // surface.
        VAIQMatrixBufferH264 iq{};
        std::memset(&iq, 16, sizeof(iq));
        VABufferID iq_buf = VA_INVALID_ID;
        if (vaCreateBuffer(dpy_, context_,
                            VAIQMatrixBufferType,
                            sizeof(iq), 1, &iq, &iq_buf)
            != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(dpy_, pic_buf);
            vaDestroyBuffer(dpy_, slice_buf);
            vaDestroyBuffer(dpy_, data_buf);
            return;
        }

        VAStatus bs = vaBeginPicture(dpy_, context_, curr_surface);
        if (bs != VA_STATUS_SUCCESS) {
            if (!first_err_logged_) {
                std::fprintf(stderr,
                    "unio-pipe: vaBeginPicture: %s\n",
                    vaErrorStr(bs));
                first_err_logged_ = true;
            }
            vaDestroyBuffer(dpy_, pic_buf);
            vaDestroyBuffer(dpy_, iq_buf);
            vaDestroyBuffer(dpy_, slice_buf);
            vaDestroyBuffer(dpy_, data_buf);
            return;
        }
        // ffmpeg / libav submit each buffer type in its own
        // vaRenderPicture call. A batched list works on some
        // drivers but not all — Intel iHD in particular is
        // picky here.
        VABufferID one;
        VAStatus rs = VA_STATUS_SUCCESS;
        one = pic_buf;
        rs = vaRenderPicture(dpy_, context_, &one, 1);
        one = iq_buf;
        if (rs == VA_STATUS_SUCCESS)
            rs = vaRenderPicture(dpy_, context_, &one, 1);
        one = slice_buf;
        if (rs == VA_STATUS_SUCCESS)
            rs = vaRenderPicture(dpy_, context_, &one, 1);
        one = data_buf;
        if (rs == VA_STATUS_SUCCESS)
            rs = vaRenderPicture(dpy_, context_, &one, 1);
        VAStatus es = vaEndPicture(dpy_, context_);
        VAStatus ss = vaSyncSurface(dpy_, curr_surface);
        if ((rs != VA_STATUS_SUCCESS || es != VA_STATUS_SUCCESS
             || ss != VA_STATUS_SUCCESS) && !first_err_logged_) {
            std::fprintf(stderr,
                "unio-pipe: va decode error render=%s end=%s sync=%s\n",
                vaErrorStr(rs), vaErrorStr(es), vaErrorStr(ss));
            first_err_logged_ = true;
        }

        prev_ref_surface_ = curr_surface;
        prev_frame_num_ = sh.frame_num;
        frame_count_++;

        if (on_frame_) {
            DecodedFrame df;
            df.surface_handle =
                static_cast<std::uintptr_t>(curr_surface);
            df.native_device =
                reinterpret_cast<std::uintptr_t>(dpy_);
            df.width = static_cast<std::uint32_t>(width_);
            df.height = static_cast<std::uint32_t>(height_);
            df.decode_done_monotonic_ns = NowNs();
            df.frame_id = pending_frame_id_;
            df.capture_monotonic_ns = pending_capture_ns_;
            df.key_frame = is_idr;
            on_frame_(df);
            pending_frame_id_ = 0;
            pending_capture_ns_ = 0;
        }
    }

    bool OpenContext() {
        VAConfigAttrib attr{};
        attr.type = VAConfigAttribRTFormat;
        attr.value = VA_RT_FORMAT_YUV420;
        VAProfile profile =
            sps_.profile_idc == 66
                ? VAProfileH264ConstrainedBaseline
                : sps_.profile_idc == 77
                    ? VAProfileH264Main
                    : VAProfileH264High;
        if (vaCreateConfig(dpy_, profile, VAEntrypointVLD,
                            &attr, 1, &config_)
            != VA_STATUS_SUCCESS) {
            return false;
        }
        surfaces_.resize(kSurfaceCount, VA_INVALID_ID);
        VASurfaceAttrib sattr{};
        sattr.type = VASurfaceAttribPixelFormat;
        sattr.flags = VA_SURFACE_ATTRIB_SETTABLE;
        sattr.value.type = VAGenericValueTypeInteger;
        sattr.value.value.i = VA_FOURCC_NV12;
        if (vaCreateSurfaces(dpy_, VA_RT_FORMAT_YUV420,
                              width_, height_,
                              surfaces_.data(), kSurfaceCount,
                              &sattr, 1) != VA_STATUS_SUCCESS) {
            return false;
        }
        if (vaCreateContext(dpy_, config_, width_, height_,
                             VA_PROGRESSIVE, surfaces_.data(),
                             kSurfaceCount, &context_)
            != VA_STATUS_SUCCESS) {
            return false;
        }
        have_context_ = true;
        std::fprintf(stderr,
                     "unio-pipe: decoder context %dx%d "
                     "profile=%d NV12\n",
                     width_, height_, static_cast<int>(profile));
        return true;
    }

    VABufferID BuildPictureParam(VASurfaceID curr,
                                  const ParsedSliceHeader& sh,
                                  bool is_idr) {
        VAPictureParameterBufferH264 p{};
        p.CurrPic.picture_id = curr;
        p.CurrPic.frame_idx = sh.frame_num;
        p.CurrPic.flags = 0;
        p.CurrPic.TopFieldOrderCnt = sh.frame_num * 2;
        p.CurrPic.BottomFieldOrderCnt = sh.frame_num * 2;
        for (auto& rp : p.ReferenceFrames) {
            rp.picture_id = VA_INVALID_SURFACE;
            rp.flags = VA_PICTURE_H264_INVALID;
        }
        if (!is_idr && prev_ref_surface_ != VA_INVALID_SURFACE) {
            auto& r0 = p.ReferenceFrames[0];
            r0.picture_id = prev_ref_surface_;
            r0.frame_idx = prev_frame_num_;
            r0.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            r0.TopFieldOrderCnt = prev_frame_num_ * 2;
            r0.BottomFieldOrderCnt = prev_frame_num_ * 2;
        }
        p.picture_width_in_mbs_minus1 =
            static_cast<std::uint16_t>(sps_.pic_width_in_mbs - 1);
        p.picture_height_in_mbs_minus1 =
            static_cast<std::uint16_t>(sps_.pic_height_in_mbs - 1);
        p.bit_depth_luma_minus8 = 0;
        p.bit_depth_chroma_minus8 = 0;
        p.num_ref_frames =
            static_cast<std::uint8_t>(sps_.num_ref_frames);
        p.seq_fields.bits.chroma_format_idc = sps_.chroma_format_idc;
        p.seq_fields.bits.frame_mbs_only_flag = sps_.frame_mbs_only_flag ? 1 : 0;
        p.seq_fields.bits.pic_order_cnt_type = sps_.pic_order_cnt_type;
        p.seq_fields.bits.log2_max_frame_num_minus4 =
            sps_.log2_max_frame_num_minus4;
        p.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
        p.seq_fields.bits.direct_8x8_inference_flag =
            sps_.direct_8x8_inference_flag ? 1 : 0;
        // num_slice_groups_minus1 / slice_group_map_type /
        // slice_group_change_rate_minus1 are deprecated in current
        // libva headers (ASO/FMO never got real HW support). Zero-
        // init handles them; don't touch deprecated fields directly.
        p.pic_init_qp_minus26 = pps_.pic_init_qp_minus26;
        p.pic_init_qs_minus26 = pps_.pic_init_qs_minus26;
        p.chroma_qp_index_offset = pps_.chroma_qp_index_offset;
        p.second_chroma_qp_index_offset = pps_.chroma_qp_index_offset;
        p.pic_fields.bits.entropy_coding_mode_flag =
            pps_.entropy_coding_mode_flag ? 1 : 0;
        p.pic_fields.bits.weighted_pred_flag =
            pps_.weighted_pred_flag ? 1 : 0;
        p.pic_fields.bits.weighted_bipred_idc =
            pps_.weighted_bipred_idc;
        p.pic_fields.bits.transform_8x8_mode_flag = 0;
        p.pic_fields.bits.field_pic_flag = 0;
        p.pic_fields.bits.constrained_intra_pred_flag =
            pps_.constrained_intra_pred_flag ? 1 : 0;
        p.pic_fields.bits.pic_order_present_flag =
            pps_.bottom_field_pic_order_in_frame_present_flag ? 1 : 0;
        p.pic_fields.bits.deblocking_filter_control_present_flag =
            pps_.deblocking_filter_control_present_flag ? 1 : 0;
        p.pic_fields.bits.redundant_pic_cnt_present_flag =
            pps_.redundant_pic_cnt_present_flag ? 1 : 0;
        p.pic_fields.bits.reference_pic_flag = 1;
        p.frame_num = static_cast<std::uint16_t>(sh.frame_num);

        VABufferID buf = VA_INVALID_ID;
        if (vaCreateBuffer(dpy_, context_,
                            VAPictureParameterBufferType,
                            sizeof(p), 1, &p, &buf)
            != VA_STATUS_SUCCESS) {
            return VA_INVALID_ID;
        }
        return buf;
    }

    VABufferID BuildSliceParam(const ParsedSliceHeader& sh,
                                std::size_t nal_full_len) {
        VASliceParameterBufferH264 s{};
        // slice_data_size counts the full buffer we pass —
        // start code (4) + NAL header (1) + slice body.
        s.slice_data_size =
            static_cast<std::uint32_t>(4 + nal_full_len);
        s.slice_data_offset = 0;
        s.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        // slice_data_bit_offset: bit offset from the start of
        // slice_data_buffer to the first bit of slice_data().
        //   4 bytes start code + 1 byte NAL header + parsed
        //   slice_header bits (from our RBSP-space parser, which
        //   matches EBSP for our short headers with no 0x00 0x00
        //   sequences).
        s.slice_data_bit_offset =
            static_cast<std::uint16_t>((4 + 1) * 8
                                        + sh.slice_data_bit_offset);
        s.first_mb_in_slice =
            static_cast<std::uint16_t>(sh.first_mb_in_slice);
        s.slice_type = static_cast<std::uint8_t>(sh.slice_type_raw);
        s.direct_spatial_mv_pred_flag = 0;
        s.num_ref_idx_l0_active_minus1 =
            static_cast<std::uint8_t>(sh.num_ref_idx_l0_active_minus1);
        s.num_ref_idx_l1_active_minus1 = 0;
        s.cabac_init_idc = 0;
        s.slice_qp_delta = sh.slice_qp_delta;
        s.disable_deblocking_filter_idc =
            static_cast<std::uint8_t>(sh.disable_deblocking_filter_idc);
        s.slice_alpha_c0_offset_div2 =
            static_cast<std::int8_t>(sh.slice_alpha_c0_offset_div2);
        s.slice_beta_offset_div2 =
            static_cast<std::int8_t>(sh.slice_beta_offset_div2);
        for (auto& r : s.RefPicList0) {
            r.picture_id = VA_INVALID_SURFACE;
            r.flags = VA_PICTURE_H264_INVALID;
        }
        for (auto& r : s.RefPicList1) {
            r.picture_id = VA_INVALID_SURFACE;
            r.flags = VA_PICTURE_H264_INVALID;
        }
        if (sh.slice_type != 2    // not I
            && prev_ref_surface_ != VA_INVALID_SURFACE) {
            auto& r = s.RefPicList0[0];
            r.picture_id = prev_ref_surface_;
            r.frame_idx = prev_frame_num_;
            r.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            r.TopFieldOrderCnt = prev_frame_num_ * 2;
            r.BottomFieldOrderCnt = prev_frame_num_ * 2;
        }
        s.luma_log2_weight_denom = 0;
        s.chroma_log2_weight_denom = 0;
        VABufferID buf = VA_INVALID_ID;
        if (vaCreateBuffer(dpy_, context_,
                            VASliceParameterBufferType,
                            sizeof(s), 1, &s, &buf)
            != VA_STATUS_SUCCESS) {
            return VA_INVALID_ID;
        }
        return buf;
    }

    void DestroyContext() {
        if (context_ != VA_INVALID_ID) {
            vaDestroyContext(dpy_, context_);
            context_ = VA_INVALID_ID;
        }
        if (!surfaces_.empty() && surfaces_[0] != VA_INVALID_ID) {
            vaDestroySurfaces(dpy_, surfaces_.data(),
                              static_cast<int>(surfaces_.size()));
        }
        surfaces_.clear();
        if (config_ != VA_INVALID_ID) {
            vaDestroyConfig(dpy_, config_);
            config_ = VA_INVALID_ID;
        }
        have_context_ = false;
        prev_ref_surface_ = VA_INVALID_SURFACE;
    }

    void Teardown() {
        DestroyContext();
        if (dpy_) {
            vaTerminate(dpy_);
            dpy_ = nullptr;
        }
        if (drm_fd_ >= 0) {
            ::close(drm_fd_);
            drm_fd_ = -1;
        }
    }

    Config cfg_{};
    FrameReady on_frame_;
    int drm_fd_ = -1;
    VADisplay dpy_ = nullptr;
    VAConfigID config_ = VA_INVALID_ID;
    VAContextID context_ = VA_INVALID_ID;
    std::vector<VASurfaceID> surfaces_;
    bool have_sps_ = false;
    bool have_pps_ = false;
    bool have_context_ = false;
    ParsedSps sps_{};
    ParsedPps pps_{};
    int width_ = 0;
    int height_ = 0;
    int surface_idx_ = 0;
    VASurfaceID prev_ref_surface_ = VA_INVALID_SURFACE;
    int prev_frame_num_ = 0;
    int frame_num_mod_ = 0;
    std::uint64_t frame_count_ = 0;
    std::uint64_t skipped_nonidr_ = 0;
    unsigned debug_nal_count_ = 0;
    std::uint64_t pending_frame_id_ = 0;
    std::uint64_t pending_capture_ns_ = 0;
    bool first_err_logged_ = false;
    std::vector<std::uint8_t> slice_scratch_;
};

}  // namespace

std::unique_ptr<Decoder> MakeVaapiDecoder() {
    return std::make_unique<VaapiDecoder>();
}

}  // namespace unio

#endif  // __linux__
