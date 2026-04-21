// H.264 Annex-B parsing + building primitives. Extracted from
// encoder_vaapi.cpp + decoder_vaapi.cpp so the Windows D3D11VA
// decoder + any future NVDEC or MF wrapper can share the same
// code path. Every field we parse or emit is bit-identical to
// what the Linux encoder produces — the loopback test only stays
// byte-perfect because the reader and writer are symmetrical.
//
// See include/h264_parse.h for the public interface; this file
// is only the implementations plus a couple of private helpers.

#include "h264_parse.h"

#include <cstring>

namespace unio {

// ---------------------------------------------------------------
// BitReader
// ---------------------------------------------------------------

std::uint32_t BitReader::ReadBit() {
    if (pos_ >= len_ * 8) return 0;
    std::uint8_t byte = data_[pos_ / 8];
    std::uint32_t b = (byte >> (7 - (pos_ % 8))) & 1;
    ++pos_;
    return b;
}

std::uint32_t BitReader::U(int nbits) {
    std::uint32_t v = 0;
    for (int i = 0; i < nbits; ++i) v = (v << 1) | ReadBit();
    return v;
}

std::uint32_t BitReader::Ue() {
    int leading = 0;
    while (pos_ < len_ * 8 && ReadBit() == 0) ++leading;
    std::uint32_t v = 0;
    for (int i = 0; i < leading; ++i) v = (v << 1) | ReadBit();
    return ((1u << leading) - 1) + v;
}

std::int32_t BitReader::Se() {
    std::uint32_t u = Ue();
    return (u & 1) ? static_cast<std::int32_t>((u + 1) / 2)
                    : -static_cast<std::int32_t>(u / 2);
}

// ---------------------------------------------------------------
// BitWriter
// ---------------------------------------------------------------

void BitWriter::FlushByte() {
    bytes_.push_back(bit_buf_ & 0xFF);
    bit_buf_ = 0;
    bit_count_ = 0;
}

void BitWriter::WriteBits(std::uint32_t value, int nbits) {
    for (int i = nbits - 1; i >= 0; --i) {
        bit_buf_ = (bit_buf_ << 1) | ((value >> i) & 1);
        ++bit_count_;
        if (bit_count_ == 8) FlushByte();
    }
}

void BitWriter::WriteUe(std::uint32_t n) {
    std::uint32_t m = n + 1;
    int prefix = 0;
    while ((1u << (prefix + 1)) <= m) ++prefix;
    WriteBits(0, prefix);
    WriteBits(m, prefix + 1);
}

void BitWriter::WriteSe(std::int32_t n) {
    std::uint32_t u = (n <= 0)
        ? static_cast<std::uint32_t>(-2 * n)
        : static_cast<std::uint32_t>(2 * n - 1);
    WriteUe(u);
}

void BitWriter::WriteRbspTrailing() {
    WriteBits(1, 1);
    while (bit_count_ != 0) WriteBits(0, 1);
}

std::vector<std::uint8_t> BitWriter::EmitAnnexB(std::uint8_t nal_header) {
    std::vector<std::uint8_t> out;
    out.reserve(bytes_.size() + 8);
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
    out.push_back(nal_header);
    if (bit_count_ != 0) WriteBits(0, 8 - bit_count_);
    int zero_run = 0;
    for (auto b : bytes_) {
        if (zero_run >= 2 && b <= 0x03) {
            out.push_back(0x03);
            zero_run = 0;
        }
        out.push_back(b);
        zero_run = (b == 0) ? zero_run + 1 : 0;
    }
    return out;
}

// ---------------------------------------------------------------
// Annex-B scanning + emulation-prevention stripping
// ---------------------------------------------------------------

std::vector<std::uint8_t> StripEmulationPrevention(
        const std::uint8_t* ebsp, std::size_t n) {
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(n);
    int zero_run = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t b = ebsp[i];
        if (zero_run >= 2 && b == 0x03) { zero_run = 0; continue; }
        rbsp.push_back(b);
        zero_run = (b == 0) ? zero_run + 1 : 0;
    }
    return rbsp;
}

std::vector<NalSpan> ScanAnnexB(const std::uint8_t* bytes,
                                std::size_t len) {
    std::vector<NalSpan> out;
    std::size_t i = 0;
    while (i + 3 < len) {
        const bool sc4 = (bytes[i] == 0 && bytes[i+1] == 0
                      && bytes[i+2] == 0 && bytes[i+3] == 1);
        const bool sc3 = (!sc4 && bytes[i] == 0 && bytes[i+1] == 0
                      && bytes[i+2] == 1);
        if (sc4 || sc3) {
            const std::size_t sc_len = sc4 ? 4 : 3;
            const std::size_t nal_start = i + sc_len;
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

// ---------------------------------------------------------------
// SPS / PPS parsers
// ---------------------------------------------------------------

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
            int count = (out.chroma_format_idc == 3) ? 12 : 8;
            for (int i = 0; i < count; ++i) {
                if (r.U(1)) {
                    // Scaling lists unsupported in our streams.
                }
            }
        }
    }
    out.log2_max_frame_num_minus4 = static_cast<int>(r.Ue());
    out.pic_order_cnt_type = static_cast<int>(r.Ue());
    if (out.pic_order_cnt_type == 0) {
        r.Ue();                                    // log2_max_poc_lsb_m4
    } else if (out.pic_order_cnt_type == 1) {
        r.U(1);
        r.Se();
        r.Se();
        std::uint32_t cycles = r.Ue();
        for (std::uint32_t i = 0; i < cycles; ++i) r.Se();
    }
    out.num_ref_frames = static_cast<int>(r.Ue());
    r.U(1);                                        // gaps
    out.pic_width_in_mbs = static_cast<int>(r.Ue()) + 1;
    out.pic_height_in_mbs = static_cast<int>(r.Ue()) + 1;
    out.frame_mbs_only_flag = r.U(1) != 0;
    if (!out.frame_mbs_only_flag) r.U(1);
    out.direct_8x8_inference_flag = r.U(1) != 0;
    if (r.U(1)) {                                  // frame_cropping
        r.Ue(); out.crop_right_offset = static_cast<int>(r.Ue());
        r.Ue(); out.crop_bottom_offset = static_cast<int>(r.Ue());
    }
    return true;
}

bool ParsePps(const std::uint8_t* rbsp, std::size_t len,
              ParsedPps& out) {
    if (len < 1) return false;
    BitReader r(rbsp, len);
    out.pic_parameter_set_id = static_cast<int>(r.Ue());
    out.seq_parameter_set_id = static_cast<int>(r.Ue());
    out.entropy_coding_mode_flag = r.U(1) != 0;
    out.bottom_field_pic_order_in_frame_present_flag = r.U(1) != 0;
    out.num_slice_groups_minus1 = static_cast<int>(r.Ue());
    if (out.num_slice_groups_minus1 > 0) return false;
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

bool ParseSliceHeader(const std::uint8_t* rbsp, std::size_t len,
                      bool is_idr, const ParsedSps& sps,
                      const ParsedPps& pps,
                      ParsedSliceHeader& out) {
    BitReader r(rbsp, len);
    out.first_mb_in_slice = static_cast<int>(r.Ue());
    out.slice_type_raw = static_cast<int>(r.Ue());
    out.slice_type = out.slice_type_raw % 5;
    out.pic_parameter_set_id = static_cast<int>(r.Ue());
    const int fn_bits = sps.log2_max_frame_num_minus4 + 4;
    out.frame_num = static_cast<int>(r.U(fn_bits));
    if (!sps.frame_mbs_only_flag) {
        if (r.U(1)) r.U(1);
    }
    if (is_idr) {
        out.idr_pic_id = static_cast<int>(r.Ue());
    }
    if (sps.pic_order_cnt_type == 0) return false;
    if (sps.pic_order_cnt_type == 1) return false;
    if (pps.redundant_pic_cnt_present_flag) r.Ue();
    const bool is_p = (out.slice_type == 0);
    const bool is_i = (out.slice_type == 2);
    if (!is_i) {
        out.num_ref_idx_active_override_flag = r.U(1) != 0;
        if (out.num_ref_idx_active_override_flag) {
            out.num_ref_idx_l0_active_minus1 = static_cast<int>(r.Ue());
        } else {
            out.num_ref_idx_l0_active_minus1 =
                pps.num_ref_idx_l0_default_active_minus1;
        }
    }
    if (!is_i) {
        if (r.U(1)) {
            while (true) {
                std::uint32_t idc = r.Ue();
                if (idc == 3) break;
                r.Ue();
            }
        }
    }
    if ((pps.weighted_pred_flag && is_p)
        || (pps.weighted_bipred_idc == 1 && out.slice_type == 1)) {
        return false;
    }
    if (is_idr) {
        r.U(1); r.U(1);
    } else {
        if (r.U(1)) {
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
    if (pps.entropy_coding_mode_flag && !is_i) r.Ue();
    out.slice_qp_delta = r.Se();
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

// ---------------------------------------------------------------
// SPS / PPS / slice-header builders
// ---------------------------------------------------------------

PackedHeader BuildSps(int profile_idc, int level_idc,
                      int mbs_w, int mbs_h,
                      int px_w, int px_h) {
    BitWriter w;
    w.WriteBits(profile_idc, 8);
    w.WriteBits(profile_idc == 66 ? 0x40 : 0, 8);
    w.WriteBits(level_idc, 8);
    w.WriteUe(0);                   // seq_parameter_set_id
    const bool has_high_profile_chroma_block =
        (profile_idc == 100 || profile_idc == 110
         || profile_idc == 122 || profile_idc == 244
         || profile_idc == 44  || profile_idc == 83
         || profile_idc == 86  || profile_idc == 118
         || profile_idc == 128 || profile_idc == 138
         || profile_idc == 139 || profile_idc == 134
         || profile_idc == 135);
    if (has_high_profile_chroma_block) {
        w.WriteUe(1);
        w.WriteUe(0);
        w.WriteUe(0);
        w.WriteBits(0, 1);
        w.WriteBits(0, 1);
    }
    w.WriteUe(0);                   // log2_max_frame_num_minus4
    w.WriteUe(2);                   // pic_order_cnt_type = 2
    w.WriteUe(2);                   // num_ref_frames
    w.WriteBits(0, 1);              // gaps
    w.WriteUe(mbs_w - 1);
    w.WriteUe(mbs_h - 1);
    w.WriteBits(1, 1);              // frame_mbs_only_flag
    w.WriteBits(1, 1);              // direct_8x8_inference_flag
    const bool crop = (px_w & 15) || (px_h & 15);
    w.WriteBits(crop ? 1 : 0, 1);
    if (crop) {
        w.WriteUe(0);
        w.WriteUe((mbs_w * 16 - px_w) / 2);
        w.WriteUe(0);
        w.WriteUe((mbs_h * 16 - px_h) / 2);
    }
    w.WriteBits(0, 1);              // vui_parameters_present_flag
    w.WriteRbspTrailing();
    auto bytes = w.EmitAnnexB(0x67);
    // Compute bit_length before moving bytes — {std::move(b),
    // b.size()*8} reads the moved-from vector and returns 0.
    const std::uint32_t bits =
        static_cast<std::uint32_t>(bytes.size()) * 8;
    return {std::move(bytes), bits};
}

PackedHeader BuildPps(bool cabac, int pic_init_qp) {
    BitWriter w;
    w.WriteUe(0);                   // pic_parameter_set_id
    w.WriteUe(0);                   // seq_parameter_set_id
    w.WriteBits(cabac ? 1 : 0, 1);
    w.WriteBits(0, 1);              // bottom_field_...
    w.WriteUe(0);                   // num_slice_groups_m1
    w.WriteUe(0);                   // num_ref_idx_l0_default_m1
    w.WriteUe(0);                   // num_ref_idx_l1_default_m1
    w.WriteBits(0, 1);              // weighted_pred
    w.WriteBits(0, 2);              // weighted_bipred_idc
    w.WriteSe(pic_init_qp - 26);
    w.WriteSe(0);                   // pic_init_qs_m26
    w.WriteSe(0);                   // chroma_qp_index_offset
    w.WriteBits(1, 1);              // deblocking_filter_control_present
    w.WriteBits(0, 1);              // constrained_intra_pred
    w.WriteBits(0, 1);              // redundant_pic_cnt_present
    w.WriteRbspTrailing();
    auto bytes = w.EmitAnnexB(0x68);
    const std::uint32_t bits =
        static_cast<std::uint32_t>(bytes.size()) * 8;
    return {std::move(bytes), bits};
}

PackedHeader BuildSliceHeader(bool is_idr, int frame_num,
                               int idr_pic_id) {
    BitWriter w;
    w.WriteUe(0);                    // first_mb_in_slice
    w.WriteUe(is_idr ? 7u : 5u);     // slice_type (all-I / all-P)
    w.WriteUe(0);                    // pic_parameter_set_id
    w.WriteBits(frame_num & 0xF, 4); // u(log2_max_frame_num_m4 + 4)
    if (is_idr) {
        w.WriteUe(idr_pic_id);
        w.WriteBits(0, 1);           // no_output_of_prior_pics_flag
        w.WriteBits(0, 1);           // long_term_reference_flag
    } else {
        w.WriteBits(1, 1);           // num_ref_idx_active_override_flag
        w.WriteUe(0);                // num_ref_idx_l0_active_minus1
        w.WriteBits(0, 1);           // ref_pic_list_modification_flag_l0
        w.WriteBits(0, 1);           // adaptive_ref_pic_marking_mode_flag
    }
    w.WriteSe(0);                    // slice_qp_delta
    w.WriteUe(0);                    // disable_deblocking_filter_idc
    w.WriteSe(0);                    // slice_alpha_c0_offset_div2
    w.WriteSe(0);                    // slice_beta_offset_div2
    const std::uint8_t nal_hdr = is_idr ? 0x65u : 0x61u;
    const std::uint32_t header_bits =
        static_cast<std::uint32_t>(w.BitCount());
    auto bytes = w.EmitAnnexB(nal_hdr);
    const std::uint32_t bit_length = (4 + 1) * 8 + header_bits;
    return {std::move(bytes), bit_length};
}

}  // namespace unio
