#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// H.264 Annex-B bitstream helpers shared by the VA-API and the
// D3D11VA decoders + the VA-API encoder. Everything is
// self-contained: no OS, vendor, or libva / d3d11 dependencies —
// the parsers run on whatever NAL bytes the transport delivered.
//
// Scope covered (intentionally narrow — matches what our encoder
// emits):
//   * Baseline / Main / Constrained-Baseline profile SPS + PPS
//   * CAVLC slice headers (no CABAC parsing, we don't ship it)
//   * pic_order_cnt_type = 2, frame_mbs_only = 1, weighted_pred = 0
//   * Single slice per picture, single reference frame
//
// Out of scope (rejected by the parsers with a false return): B
// slices, field pictures, FMO/ASO, weighted prediction tables,
// adaptive ref-pic marking loops with more than the simple
// "one short-term ref" we emit.

namespace unio {

// H.264 NAL unit types from Recommendation ITU-T H.264,
// subclause 7.4.1. The values are the 5-bit `nal_unit_type`
// field, not the full header byte — strip with `hdr & 0x1F`.
constexpr std::uint8_t kNalNonIdrSlice = 1;
constexpr std::uint8_t kNalIdrSlice    = 5;
constexpr std::uint8_t kNalSei         = 6;
constexpr std::uint8_t kNalSps         = 7;
constexpr std::uint8_t kNalPps         = 8;

// MSB-first bitstream reader over a byte buffer. Out-of-range
// reads return zero rather than throwing — the parsers above
// range-check their own inputs.
class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t len)
        : data_(data), len_(len) {}
    std::uint32_t U(int nbits);
    std::uint32_t Ue();
    std::int32_t  Se();
    std::size_t   BitPos() const { return pos_; }

private:
    std::uint32_t ReadBit();

    const std::uint8_t* data_;
    std::size_t len_;
    std::size_t pos_ = 0;
};

// MSB-first bitstream writer with exp-Golomb helpers and Annex-B
// emission + emulation-prevention byte insertion. Matches the
// BitReader shape so write/read are exact inverses.
class BitWriter {
public:
    void WriteBits(std::uint32_t value, int nbits);
    void WriteUe(std::uint32_t n);
    void WriteSe(std::int32_t n);
    // Stop bit + zero pad to byte boundary. Call on SPS / PPS
    // builders; slice-header builders MUST NOT call this (slice
    // data continues in the same byte).
    void WriteRbspTrailing();
    int  BitCount() const {
        return static_cast<int>(bytes_.size()) * 8 + bit_count_;
    }
    // Emit [00 00 00 01] + nal_header + bytes_ with emulation
    // prevention. Pads a partial last byte with zeros so the
    // returned vector is byte-aligned (slice-header callers pass
    // the exact bit_length separately to the driver).
    std::vector<std::uint8_t> EmitAnnexB(std::uint8_t nal_header);

private:
    void FlushByte();

    std::vector<std::uint8_t> bytes_;
    std::uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
};

// One NAL unit pointer back into a byte buffer. Offset skips
// the start code so the first byte at offset is the NAL header.
struct NalSpan {
    std::size_t offset;
    std::size_t length;
};
std::vector<NalSpan> ScanAnnexB(const std::uint8_t* bytes,
                                std::size_t len);

// EBSP → RBSP. Returns a fresh buffer; caller owns the result.
std::vector<std::uint8_t> StripEmulationPrevention(
    const std::uint8_t* ebsp, std::size_t n);

// Parsed structures the decoder uses to build VA / D3D11 picture
// + slice buffers. Fields mirror the H.264 spec field names
// rather than the VA-API struct field names — the decoder is
// responsible for translating.
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

struct ParsedSliceHeader {
    int first_mb_in_slice = 0;
    int slice_type = 0;          // canonical (% 5)
    int slice_type_raw = 0;      // as sent (may be 5-9)
    int pic_parameter_set_id = 0;
    int frame_num = 0;
    int idr_pic_id = 0;
    int slice_qp_delta = 0;
    int disable_deblocking_filter_idc = 0;
    int slice_alpha_c0_offset_div2 = 0;
    int slice_beta_offset_div2 = 0;
    int num_ref_idx_l0_active_minus1 = 0;
    bool num_ref_idx_active_override_flag = false;
    // Bit position (from the NAL header byte, exclusive) at which
    // slice_data() begins. D3D11VA and VA-API both need it.
    std::size_t slice_data_bit_offset = 0;
};

bool ParseSps(const std::uint8_t* rbsp, std::size_t len,
              ParsedSps& out);
bool ParsePps(const std::uint8_t* rbsp, std::size_t len,
              ParsedPps& out);
bool ParseSliceHeader(const std::uint8_t* rbsp, std::size_t len,
                      bool is_idr, const ParsedSps& sps,
                      const ParsedPps& pps,
                      ParsedSliceHeader& out);

// Builders matching the parsers — used by the encoder to emit
// SPS / PPS / slice-header as packed headers. bit_length is the
// exact count the driver needs to know where slice_data() starts.
struct PackedHeader {
    std::vector<std::uint8_t> bytes;
    std::uint32_t bit_length = 0;
};

PackedHeader BuildSps(int profile_idc, int level_idc,
                      int mbs_w, int mbs_h,
                      int px_w, int px_h);
PackedHeader BuildPps(bool cabac, int pic_init_qp);
PackedHeader BuildSliceHeader(bool is_idr, int frame_num,
                               int idr_pic_id);

}  // namespace unio
