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

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count());
}

// Strip H.264 emulation prevention bytes (0x03 inserted after
// 00 00 to disambiguate from start codes). Returns the RBSP byte
// stream the slice/SPS/PPS parsers operate on.
std::vector<std::uint8_t> StripEmulationPrevention(
        const std::uint8_t* ebsp, std::size_t n) {
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(n);
    int zero_run = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t b = ebsp[i];
        if (zero_run >= 2 && b == 0x03) {
            zero_run = 0;
            continue;
        }
        rbsp.push_back(b);
        zero_run = (b == 0) ? zero_run + 1 : 0;
    }
    return rbsp;
}

// Minimal bit reader: big-endian bit stream over a byte buffer,
// with exp-Golomb unsigned/signed helpers. Matches the encoder's
// BitWriter shape from encoder_vaapi.cpp so read / write are
// exact inverses.
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t len)
        : data_(data), len_(len) {}

    std::uint32_t U(int nbits) {
        std::uint32_t v = 0;
        for (int i = 0; i < nbits; ++i) {
            v = (v << 1) | ReadBit();
        }
        return v;
    }
    std::uint32_t Ue() {
        int leading = 0;
        while (pos_ < len_ * 8 && ReadBit() == 0) ++leading;
        std::uint32_t v = 0;
        for (int i = 0; i < leading; ++i) {
            v = (v << 1) | ReadBit();
        }
        return ((1u << leading) - 1) + v;
    }
    std::int32_t Se() {
        std::uint32_t u = Ue();
        return (u & 1) ? static_cast<std::int32_t>((u + 1) / 2)
                        : -static_cast<std::int32_t>(u / 2);
    }
    std::size_t BitPos() const { return pos_; }

private:
    std::uint32_t ReadBit() {
        if (pos_ >= len_ * 8) return 0;
        std::uint8_t byte = data_[pos_ / 8];
        std::uint32_t b = (byte >> (7 - (pos_ % 8))) & 1;
        ++pos_;
        return b;
    }

    const std::uint8_t* data_;
    std::size_t len_;
    std::size_t pos_ = 0;
};

// NAL unit types from H.264 subclause 7.4.1.
constexpr std::uint8_t kNalNonIdrSlice = 1;
constexpr std::uint8_t kNalIdrSlice = 5;
constexpr std::uint8_t kNalSps = 7;
constexpr std::uint8_t kNalPps = 8;

// Scanner for Annex-B NAL boundaries. Returns vector of
// {nal_start, nal_len} pairs where nal_start skips the start code
// so the first byte is the NAL header.
struct NalSpan {
    std::size_t offset;
    std::size_t length;
};
std::vector<NalSpan> ScanAnnexB(const std::uint8_t* bytes,
                                std::size_t len) {
    std::vector<NalSpan> out;
    std::size_t i = 0;
    while (i + 3 < len) {
        // 4-byte start code
        if (bytes[i] == 0 && bytes[i+1] == 0
            && bytes[i+2] == 0 && bytes[i+3] == 1) {
            std::size_t nal_start = i + 4;
            i = nal_start;
            while (i + 3 < len
                   && !(bytes[i] == 0 && bytes[i+1] == 0
                        && (bytes[i+2] == 1
                            || (bytes[i+2] == 0
                                && bytes[i+3] == 1)))) {
                ++i;
            }
            out.push_back({nal_start,
                           (i + 3 < len) ? (i - nal_start)
                                          : (len - nal_start)});
            continue;
        }
        // 3-byte start code
        if (bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 1) {
            std::size_t nal_start = i + 3;
            i = nal_start;
            while (i + 3 < len
                   && !(bytes[i] == 0 && bytes[i+1] == 0
                        && (bytes[i+2] == 1
                            || (bytes[i+2] == 0
                                && bytes[i+3] == 1)))) {
                ++i;
            }
            out.push_back({nal_start,
                           (i + 3 < len) ? (i - nal_start)
                                          : (len - nal_start)});
            continue;
        }
        ++i;
    }
    return out;
}

struct ParsedSps {
    int profile_idc = 0;
    int level_idc = 0;
    int chroma_format_idc = 1;
    int log2_max_frame_num_minus4 = 0;
    int pic_order_cnt_type = 0;
    int num_ref_frames = 1;
    int pic_width_in_mbs = 0;
    int pic_height_in_mbs = 0;
    bool frame_mbs_only_flag = true;
    bool direct_8x8_inference_flag = true;
    int crop_right_offset = 0;
    int crop_bottom_offset = 0;
};

bool ParseSps(const std::uint8_t* rbsp, std::size_t len,
              ParsedSps& out) {
    if (len < 4) return false;
    BitReader r(rbsp, len);
    out.profile_idc = static_cast<int>(r.U(8));
    r.U(8);                                        // constraints
    out.level_idc = static_cast<int>(r.U(8));
    r.Ue();                                        // seq_ps_id
    const bool high_chroma = (out.profile_idc == 100
                            || out.profile_idc == 110
                            || out.profile_idc == 122
                            || out.profile_idc == 244
                            || out.profile_idc == 44
                            || out.profile_idc == 83
                            || out.profile_idc == 86
                            || out.profile_idc == 118
                            || out.profile_idc == 128
                            || out.profile_idc == 138
                            || out.profile_idc == 139
                            || out.profile_idc == 134
                            || out.profile_idc == 135);
    if (high_chroma) {
        out.chroma_format_idc = static_cast<int>(r.Ue());
        if (out.chroma_format_idc == 3) r.U(1);    // separate colour
        r.Ue();                                    // bit_depth_luma_m8
        r.Ue();                                    // bit_depth_chroma_m8
        r.U(1);                                    // qpprime
        if (r.U(1)) {                              // scaling_matrix_present
            // Skip scaling lists — we don't support them (Intel
            // encoder doesn't emit them, neither does ours).
            int count = (out.chroma_format_idc == 3) ? 12 : 8;
            for (int i = 0; i < count; ++i) {
                if (r.U(1)) {
                    // Read + discard. Actual scaling-list parse
                    // isn't needed since we reject them above.
                }
            }
        }
    }
    out.log2_max_frame_num_minus4 =
        static_cast<int>(r.Ue());
    out.pic_order_cnt_type = static_cast<int>(r.Ue());
    if (out.pic_order_cnt_type == 0) {
        r.Ue();                                    // log2_max_poc_lsb_m4
    } else if (out.pic_order_cnt_type == 1) {
        r.U(1);                                    // always_zero_flag
        r.Se();                                    // offset_for_non_ref
        r.Se();                                    // offset_for_top_to_bottom
        std::uint32_t cycles = r.Ue();
        for (std::uint32_t i = 0; i < cycles; ++i) r.Se();
    }
    out.num_ref_frames = static_cast<int>(r.Ue());
    r.U(1);                                        // gaps
    out.pic_width_in_mbs =
        static_cast<int>(r.Ue()) + 1;
    out.pic_height_in_mbs =
        static_cast<int>(r.Ue()) + 1;
    out.frame_mbs_only_flag = r.U(1) != 0;
    if (!out.frame_mbs_only_flag) r.U(1);          // mbaff
    out.direct_8x8_inference_flag = r.U(1) != 0;
    if (r.U(1)) {                                  // frame_cropping
        r.Ue(); out.crop_right_offset = static_cast<int>(r.Ue());
        r.Ue(); out.crop_bottom_offset = static_cast<int>(r.Ue());
    }
    // VUI params ignored.
    return true;
}

struct ParsedPps {
    int pic_parameter_set_id = 0;
    int seq_parameter_set_id = 0;
    bool entropy_coding_mode_flag = false;
    bool bottom_field_pic_order_in_frame_present_flag = false;
    int num_slice_groups_minus1 = 0;
    int num_ref_idx_l0_default_active_minus1 = 0;
    int num_ref_idx_l1_default_active_minus1 = 0;
    bool weighted_pred_flag = false;
    int weighted_bipred_idc = 0;
    int pic_init_qp_minus26 = 0;
    int pic_init_qs_minus26 = 0;
    int chroma_qp_index_offset = 0;
    bool deblocking_filter_control_present_flag = false;
    bool constrained_intra_pred_flag = false;
    bool redundant_pic_cnt_present_flag = false;
};

bool ParsePps(const std::uint8_t* rbsp, std::size_t len,
              ParsedPps& out) {
    if (len < 1) return false;
    BitReader r(rbsp, len);
    out.pic_parameter_set_id = static_cast<int>(r.Ue());
    out.seq_parameter_set_id = static_cast<int>(r.Ue());
    out.entropy_coding_mode_flag = r.U(1) != 0;
    out.bottom_field_pic_order_in_frame_present_flag = r.U(1) != 0;
    out.num_slice_groups_minus1 = static_cast<int>(r.Ue());
    if (out.num_slice_groups_minus1 > 0) {
        // Skip — our encoder never emits slice groups.
        return false;
    }
    out.num_ref_idx_l0_default_active_minus1 = static_cast<int>(r.Ue());
    out.num_ref_idx_l1_default_active_minus1 = static_cast<int>(r.Ue());
    out.weighted_pred_flag = r.U(1) != 0;
    out.weighted_bipred_idc = static_cast<int>(r.U(2));
    out.pic_init_qp_minus26 = r.Se();
    out.pic_init_qs_minus26 = r.Se();
    out.chroma_qp_index_offset = r.Se();
    out.deblocking_filter_control_present_flag = r.U(1) != 0;
    out.constrained_intra_pred_flag = r.U(1) != 0;
    out.redundant_pic_cnt_present_flag = r.U(1) != 0;
    return true;
}

// Parsed slice-header state the VA slice-param buffer needs.
struct ParsedSliceHeader {
    int first_mb_in_slice = 0;
    int slice_type = 0;                           // canonical (% 5)
    int slice_type_raw = 0;                       // as sent (may be 5-9)
    int pic_parameter_set_id = 0;
    int frame_num = 0;
    int idr_pic_id = 0;
    int slice_qp_delta = 0;
    int disable_deblocking_filter_idc = 0;
    int slice_alpha_c0_offset_div2 = 0;
    int slice_beta_offset_div2 = 0;
    int num_ref_idx_l0_active_minus1 = 0;
    bool num_ref_idx_active_override_flag = false;
    std::size_t slice_data_bit_offset = 0;        // from NAL header byte
};

bool ParseSliceHeader(const std::uint8_t* rbsp, std::size_t len,
                      bool is_idr, const ParsedSps& sps,
                      const ParsedPps& pps,
                      ParsedSliceHeader& out) {
    BitReader r(rbsp, len);
    out.first_mb_in_slice = static_cast<int>(r.Ue());
    out.slice_type_raw = static_cast<int>(r.Ue());
    out.slice_type = out.slice_type_raw % 5;
    out.pic_parameter_set_id = static_cast<int>(r.Ue());
    // frame_num
    const int fn_bits = sps.log2_max_frame_num_minus4 + 4;
    out.frame_num = static_cast<int>(r.U(fn_bits));
    if (!sps.frame_mbs_only_flag) {
        if (r.U(1)) r.U(1);
    }
    if (is_idr) {
        out.idr_pic_id = static_cast<int>(r.Ue());
    }
    if (sps.pic_order_cnt_type == 0) {
        // log2_max_pic_order_cnt_lsb_minus4 isn't captured; we
        // don't support pct=0 streams (our encoder uses pct=2).
        return false;
    }
    if (sps.pic_order_cnt_type == 1) {
        return false;  // pct=1 not supported
    }
    if (pps.redundant_pic_cnt_present_flag) r.Ue();
    // slice_type 0..4 normal, 5..9 "all same"; P=0 or 5, I=2 or 7.
    const bool is_p = (out.slice_type == 0);
    const bool is_i = (out.slice_type == 2);
    if (!is_i) {
        out.num_ref_idx_active_override_flag = r.U(1) != 0;
        if (out.num_ref_idx_active_override_flag) {
            out.num_ref_idx_l0_active_minus1 =
                static_cast<int>(r.Ue());
        } else {
            out.num_ref_idx_l0_active_minus1 =
                pps.num_ref_idx_l0_default_active_minus1;
        }
    }
    // ref_pic_list_modification (not for I/SI)
    if (!is_i) {
        if (r.U(1)) {
            while (true) {
                std::uint32_t idc = r.Ue();
                if (idc == 3) break;
                r.Ue();
            }
        }
    }
    // pred_weight_table — skipped, weighted_pred=0 in our PPS
    if ((pps.weighted_pred_flag && is_p)
        || (pps.weighted_bipred_idc == 1
            && out.slice_type == 1)) {
        return false;  // not supported
    }
    // dec_ref_pic_marking — nal_ref_idc != 0 always in our stream
    if (is_idr) {
        r.U(1);  // no_output_of_prior_pics_flag
        r.U(1);  // long_term_reference_flag
    } else {
        if (r.U(1)) {
            // adaptive_ref_pic_marking_mode_flag — read mmco ops
            while (true) {
                std::uint32_t mmco = r.Ue();
                if (mmco == 0) break;
                if (mmco == 1 || mmco == 3) r.Ue();
                if (mmco == 2) r.Ue();
                if (mmco == 3 || mmco == 6) r.Ue();
                if (mmco == 4) r.Ue();
            }
        }
    }
    if (pps.entropy_coding_mode_flag && !is_i) {
        r.Ue();                                    // cabac_init_idc
    }
    out.slice_qp_delta = r.Se();
    // SP/SI not supported
    if (pps.deblocking_filter_control_present_flag) {
        out.disable_deblocking_filter_idc = static_cast<int>(r.Ue());
        if (out.disable_deblocking_filter_idc != 1) {
            out.slice_alpha_c0_offset_div2 = r.Se();
            out.slice_beta_offset_div2 = r.Se();
        }
    }
    out.slice_data_bit_offset = r.BitPos();
    return true;
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

        drm_fd_ = ::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm_fd_ < 0) {
            return "cannot open /dev/dri/renderD128";
        }
        dpy_ = vaGetDisplayDRM(drm_fd_);
        if (!dpy_) {
            Teardown();
            return "vaGetDisplayDRM failed";
        }
        int major = 0, minor = 0;
        if (VAStatus s = vaInitialize(dpy_, &major, &minor);
            s != VA_STATUS_SUCCESS) {
            Teardown();
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
            switch (type) {
                case kNalSps: HandleSps(rbsp.data(), rbsp.size()); break;
                case kNalPps: HandlePps(rbsp.data(), rbsp.size()); break;
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
        if (!ParseSps(rbsp, len, s)) return;
        sps_ = s;
        have_sps_ = true;
        // Rebuild context if picture size changed.
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
        if (!ParsePps(rbsp, len, p)) return;
        pps_ = p;
        have_pps_ = true;
    }

    void HandleSlice(const std::uint8_t* nal_full,
                     std::size_t nal_full_len,
                     const std::uint8_t* rbsp, std::size_t rbsp_len,
                     bool is_idr) {
        if (!have_sps_ || !have_pps_) return;
        if (!have_context_) {
            if (!OpenContext()) return;
        }
        ParsedSliceHeader sh{};
        if (!ParseSliceHeader(rbsp, rbsp_len, is_idr, sps_, pps_,
                              sh)) {
            return;
        }
        if (is_idr || prev_ref_surface_ == VA_INVALID_SURFACE) {
            // First GOP or recovery: need an IDR to sync.
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
        VABufferID data_buf = VA_INVALID_ID;
        VAStatus s = vaCreateBuffer(
            dpy_, context_, VASliceDataBufferType,
            static_cast<unsigned int>(nal_full_len), 1,
            const_cast<std::uint8_t*>(nal_full), &data_buf);
        if (s != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(dpy_, pic_buf);
            vaDestroyBuffer(dpy_, slice_buf);
            return;
        }

        if (vaBeginPicture(dpy_, context_, curr_surface)
            != VA_STATUS_SUCCESS) {
            vaDestroyBuffer(dpy_, pic_buf);
            vaDestroyBuffer(dpy_, slice_buf);
            vaDestroyBuffer(dpy_, data_buf);
            return;
        }
        VABufferID bufs[] = {pic_buf, slice_buf, data_buf};
        vaRenderPicture(dpy_, context_, bufs, 3);
        vaEndPicture(dpy_, context_);
        vaSyncSurface(dpy_, curr_surface);

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
            df.key_frame = is_idr;
            on_frame_(df);
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
        s.slice_data_size =
            static_cast<std::uint32_t>(nal_full_len);
        s.slice_data_offset = 0;
        s.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        // slice_data_bit_offset is measured from the NAL header
        // byte (after the start code is stripped). Our parser
        // operated on rbsp (NAL header already gone), so add 8
        // for the NAL header byte itself.
        s.slice_data_bit_offset =
            static_cast<std::uint16_t>(sh.slice_data_bit_offset + 8);
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
};

}  // namespace

std::unique_ptr<Decoder> MakeVaapiDecoder() {
    return std::make_unique<VaapiDecoder>();
}

}  // namespace unio

#endif  // __linux__
