// D3D11VA H.264 decoder — Windows sibling of decoder_vaapi.cpp.
// Annex-B packets come in via QuicInbound; the shared h264_parse
// module pulls SPS/PPS/slice header out of them; DXVA2 buffers get
// filled per frame and the output is an ID3D11Texture2D NV12.
//
// Same design choices as the VA-API decoder:
//   * IPPP with a single reference, CB / Main profile, CAVLC
//   * Full bitstream parse in process — no driver auto-parse
//   * Surface pool rotation, drop-oldest on backpressure
//
// The DXVA2 API surface is more layered than VA-API but the shape
// is the same: create a decoder + output views + a bunch of
// per-frame buffers (picture parameters, slice control, bitstream).
// The picture-parameter struct, DXVA_PicParams_H264, is a 400-byte
// bundle of every SPS/PPS flag + the current-frame + reference
// state; we translate from our ParsedSps / ParsedPps /
// ParsedSliceHeader into it.

#if !defined(_WIN32)
#include "decoder.h"
namespace unio {
std::unique_ptr<Decoder> MakeD3d11VaDecoder() { return nullptr; }
}  // namespace unio
#else

#include "decoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxva.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count());
}

// DXVA2 H.264 VLD profile GUIDs — handed verbatim to
// ID3D11VideoDevice::CheckVideoDecoderFormat. Mirror of the
// VAAPI profile enum but as GUIDs. Source: dxva.h.
// D3D11_DECODER_PROFILE_H264_VLD_NOFGT is the plain H.264 decoder
// without Film Grain Technology (we don't need FGT; it's an
// H.264 SVC / extension).
constexpr GUID kH264VldNoFgt = {
    0x1b81be68, 0xa0c7, 0x11d3,
    {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};

class D3d11VaDecoder final : public Decoder {
public:
    D3d11VaDecoder() = default;
    ~D3d11VaDecoder() override = default;

    std::optional<std::string> Init(const Config& cfg,
                                    FrameReady on_frame) override {
        cfg_ = cfg;
        on_frame_ = std::move(on_frame);

        UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL wanted[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got = {};
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            wanted, static_cast<UINT>(std::size(wanted)),
            D3D11_SDK_VERSION, &device_, &got, &ctx_);
        if (FAILED(hr)) {
            return std::string("D3D11CreateDevice failed, HR=")
                   + HrHex(hr);
        }
        // Video device is on the same D3D11 device handle —
        // QueryInterface for the video facets.
        if (FAILED(hr = device_.As(&video_device_))) {
            return std::string("no ID3D11VideoDevice, HR=")
                   + HrHex(hr);
        }
        if (FAILED(hr = ctx_.As(&video_ctx_))) {
            return std::string("no ID3D11VideoContext, HR=")
                   + HrHex(hr);
        }

        // Allow the driver's video engine to be used from any
        // D3D11 thread — our Feed() is called from the msquic
        // worker thread.
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device_.As(&mt))) {
            mt->SetMultithreadProtected(TRUE);
        }

        BOOL supported = FALSE;
        if (FAILED(video_device_->CheckVideoDecoderFormat(
                &kH264VldNoFgt, DXGI_FORMAT_NV12, &supported))
            || !supported) {
            return "H.264 VLD NOFGT + NV12 not supported by GPU";
        }

        std::fprintf(stderr,
            "unio-pipe: decoder D3D11 device up, feature_level=0x%x\n",
            static_cast<unsigned>(got));
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
            const std::uint8_t* body = nal + 1;
            std::size_t body_len = n.length - 1;
            auto rbsp = StripEmulationPrevention(body, body_len);
            switch (type) {
                case kNalSps:
                    HandleSps(rbsp.data(), rbsp.size());
                    break;
                case kNalPps:
                    HandlePps(rbsp.data(), rbsp.size());
                    break;
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

    std::string_view Name() const override { return "d3d11va-h264"; }

private:
    static constexpr int kSurfaceCount = 4;

    static std::string HrHex(HRESULT hr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08x",
                      static_cast<unsigned>(hr));
        return buf;
    }

    void HandleSps(const std::uint8_t* rbsp, std::size_t len) {
        ParsedSps s{};
        if (!ParseSps(rbsp, len, s)) return;
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
        if (!is_idr && prev_ref_idx_ < 0) {
            // First GOP or lost IDR: skip until we see one.
            ++skipped_nonidr_;
            return;
        }

        const int this_idx = frame_count_ % kSurfaceCount;
        ID3D11VideoDecoderOutputView* out_view = views_[this_idx].Get();

        // DecoderBeginFrame locks the hardware to this output view.
        // Can retry on E_PENDING — the driver is still finishing
        // another frame. Spin with a tiny sleep to keep latency
        // predictable; the MS samples do the same.
        HRESULT hr = E_PENDING;
        for (int retry = 0; retry < 100 && hr == E_PENDING; ++retry) {
            hr = video_ctx_->DecoderBeginFrame(decoder_.Get(),
                                                out_view, 0, nullptr);
            if (hr == E_PENDING) ::Sleep(1);
        }
        if (FAILED(hr)) return;

        if (!SubmitPictureParams(sh, is_idr, this_idx)) {
            video_ctx_->DecoderEndFrame(decoder_.Get());
            return;
        }
        if (!SubmitQuantizationMatrix()) {
            video_ctx_->DecoderEndFrame(decoder_.Get());
            return;
        }
        if (!SubmitBitstream(nal_full, nal_full_len)) {
            video_ctx_->DecoderEndFrame(decoder_.Get());
            return;
        }
        if (!SubmitSliceControl(nal_full_len, sh)) {
            video_ctx_->DecoderEndFrame(decoder_.Get());
            return;
        }

        // Single SubmitDecoderBuffers covers all four above. DXVA
        // lets us submit up to N buffers per BeginFrame; four
        // matches what we filled.
        D3D11_VIDEO_DECODER_BUFFER_DESC descs[4] = {};
        descs[0].BufferType = D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
        descs[0].DataSize  = sizeof(DXVA_PicParams_H264);
        descs[1].BufferType = D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
        descs[1].DataSize  = sizeof(DXVA_Qmatrix_H264);
        descs[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
        descs[2].DataSize  = last_bitstream_size_;
        descs[3].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
        descs[3].DataSize  = sizeof(DXVA_Slice_H264_Short);
        hr = video_ctx_->SubmitDecoderBuffers(
            decoder_.Get(), 4, descs);
        video_ctx_->DecoderEndFrame(decoder_.Get());
        if (FAILED(hr)) return;

        prev_ref_idx_ = this_idx;
        prev_frame_num_ = sh.frame_num;
        ++frame_count_;

        // Copy decoder-only NV12 → presenter-shareable NV12. NVIDIA's
        // D3D11VA implementation silently writes zeros into textures
        // that carry both D3D11_BIND_DECODER and BIND_SHADER_RESOURCE;
        // splitting into two pools (DECODER-only + SHADER-only) and
        // doing a GPU-to-GPU CopyResource on the same device is cheap
        // and works across every driver we target.
        ctx_->CopyResource(
            shader_textures_[this_idx].Get(),
            textures_[this_idx].Get());

        if (on_frame_) {
            DecodedFrame df;
            df.surface_handle =
                reinterpret_cast<std::uintptr_t>(
                    shader_textures_[this_idx].Get());
            df.native_device =
                reinterpret_cast<std::uintptr_t>(device_.Get());
            df.width = static_cast<std::uint32_t>(width_);
            df.height = static_cast<std::uint32_t>(height_);
            df.decode_done_monotonic_ns = NowNs();
            df.key_frame = is_idr;
            on_frame_(df);
        }
    }

    bool OpenContext() {
        D3D11_VIDEO_DECODER_DESC desc{};
        desc.Guid = kH264VldNoFgt;
        desc.SampleWidth = static_cast<UINT>(width_);
        desc.SampleHeight = static_cast<UINT>(height_);
        desc.OutputFormat = DXGI_FORMAT_NV12;

        UINT ncfg = 0;
        if (FAILED(video_device_->GetVideoDecoderConfigCount(
                &desc, &ncfg)) || ncfg == 0) {
            return false;
        }
        // Take the first advertised config. NVIDIA's GeForce driver
        // on Win10 22H2 lists nine H264_VLD_NOFGT configs: most are
        // ConfigBitstreamRaw=2, a few are raw=1. Only raw=2 actually
        // decodes — raw=1 returns SUCCESS from every submission and
        // leaves the NV12 surface zero-filled. Intel iHD and AMD
        // advertise raw=1 first, so "take config 0" ends up doing
        // the right thing on all three vendors. This mirrors
        // FFmpeg's preference in dxva2_h264.c.
        D3D11_VIDEO_DECODER_CONFIG cfg{};
        if (FAILED(video_device_->GetVideoDecoderConfig(
                &desc, 0, &cfg))) return false;
        cfg_bitstream_raw_ = cfg.ConfigBitstreamRaw;

        if (FAILED(video_device_->CreateVideoDecoder(
                &desc, &cfg, &decoder_))) {
            return false;
        }

        // NV12 decoder pool — DECODER-only bind flag. NVIDIA's
        // D3D11VA silently fails when SHADER_RESOURCE is also set
        // on the same texture; we use a parallel shader pool and
        // GPU-copy into it after each DecoderEndFrame.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(width_);
        td.Height = static_cast<UINT>(height_);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DECODER;
        for (int i = 0; i < kSurfaceCount; ++i) {
            if (FAILED(device_->CreateTexture2D(
                    &td, nullptr, &textures_[i]))) {
                return false;
            }
            D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC vd{};
            vd.DecodeProfile = kH264VldNoFgt;
            vd.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
            vd.Texture2D.ArraySlice = 0;
            if (FAILED(video_device_->CreateVideoDecoderOutputView(
                    textures_[i].Get(), &vd, &views_[i]))) {
                return false;
            }
        }

        // Presenter-shareable NV12 pool — SHADER_RESOURCE only.
        // One-to-one with the decoder pool; GPU CopyResource is
        // ~0.1 ms on a 1080p NV12 on integrated/discrete GPUs.
        D3D11_TEXTURE2D_DESC sh_td = td;
        sh_td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < kSurfaceCount; ++i) {
            if (FAILED(device_->CreateTexture2D(
                    &sh_td, nullptr, &shader_textures_[i]))) {
                return false;
            }
        }
        have_context_ = true;
        std::fprintf(stderr,
            "unio-pipe: decoder context %dx%d NV12 "
            "(ConfigBitstreamRaw=%d)\n",
            width_, height_, cfg_bitstream_raw_);
        return true;
    }

    bool SubmitPictureParams(const ParsedSliceHeader& sh,
                              bool is_idr, int this_idx) {
        UINT size = 0;
        void* mapped = nullptr;
        HRESULT hr = video_ctx_->GetDecoderBuffer(
            decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
            &size, &mapped);
        if (FAILED(hr) || !mapped || size < sizeof(DXVA_PicParams_H264)) {
            return false;
        }
        auto* p = static_cast<DXVA_PicParams_H264*>(mapped);
        std::memset(p, 0, sizeof(*p));
        p->wFrameWidthInMbsMinus1 =
            static_cast<USHORT>(sps_.pic_width_in_mbs - 1);
        p->wFrameHeightInMbsMinus1 =
            static_cast<USHORT>(sps_.pic_height_in_mbs - 1);
        p->CurrPic.Index7Bits = static_cast<UCHAR>(this_idx);
        p->CurrPic.AssociatedFlag = 0;      // top-field / frame
        p->num_ref_frames =
            static_cast<UCHAR>(sps_.num_ref_frames);
        p->frame_num =
            static_cast<USHORT>(sh.frame_num);
        p->CurrFieldOrderCnt[0] = sh.frame_num * 2;
        p->CurrFieldOrderCnt[1] = sh.frame_num * 2;
        p->bit_depth_luma_minus8 = 0;
        p->bit_depth_chroma_minus8 = 0;
        p->Reserved16Bits = 3;              // must be 3 for H.264
        p->StatusReportFeedbackNumber = static_cast<USHORT>(
            (frame_count_ + 1) & 0xFFFF);

        // Reference list — fill only slot 0 with prev ref (if any).
        for (int i = 0; i < 16; ++i) {
            p->RefFrameList[i].bPicEntry = 0xFF;  // unused
            p->FieldOrderCntList[i][0] = 0;
            p->FieldOrderCntList[i][1] = 0;
            p->FrameNumList[i] = 0;
        }
        if (!is_idr && prev_ref_idx_ >= 0) {
            p->RefFrameList[0].Index7Bits =
                static_cast<UCHAR>(prev_ref_idx_);
            p->RefFrameList[0].AssociatedFlag = 0;
            p->FieldOrderCntList[0][0] = prev_frame_num_ * 2;
            p->FieldOrderCntList[0][1] = prev_frame_num_ * 2;
            p->FrameNumList[0] =
                static_cast<USHORT>(prev_frame_num_);
            p->UsedForReferenceFlags = 0x3;   // both fields used
        }

        // Flag soup — every field we documented in our SPS/PPS
        // parsers maps to exactly one bit here.
        p->chroma_format_idc =
            static_cast<UCHAR>(sps_.chroma_format_idc);
        p->residual_colour_transform_flag = 0;
        // RefPicFlag must be 1 for any picture used as a reference
        // (IDRs, and P-slices with nal_ref_idc != 0). Our encoder
        // emits an IPPP stream where every frame is a reference, so
        // unconditionally 1. Drivers that see RefPicFlag=0 may skip
        // writing the NV12 output entirely as an optimisation.
        p->RefPicFlag = 1;
        p->frame_mbs_only_flag =
            sps_.frame_mbs_only_flag ? 1 : 0;
        p->field_pic_flag = 0;
        p->MbaffFrameFlag = 0;
        p->direct_8x8_inference_flag =
            sps_.direct_8x8_inference_flag ? 1 : 0;
        p->log2_max_frame_num_minus4 =
            static_cast<UCHAR>(sps_.log2_max_frame_num_minus4);
        p->pic_order_cnt_type =
            static_cast<UCHAR>(sps_.pic_order_cnt_type);
        p->log2_max_pic_order_cnt_lsb_minus4 = 0;
        p->delta_pic_order_always_zero_flag = 0;

        p->entropy_coding_mode_flag =
            pps_.entropy_coding_mode_flag ? 1 : 0;
        p->pic_order_present_flag =
            pps_.bottom_field_pic_order_in_frame_present_flag
                ? 1 : 0;
        p->weighted_pred_flag =
            pps_.weighted_pred_flag ? 1 : 0;
        p->weighted_bipred_idc =
            static_cast<UCHAR>(pps_.weighted_bipred_idc);
        p->transform_8x8_mode_flag = 0;
        p->MbsConsecutiveFlag = 1;
        p->deblocking_filter_control_present_flag =
            pps_.deblocking_filter_control_present_flag ? 1 : 0;
        p->redundant_pic_cnt_present_flag =
            pps_.redundant_pic_cnt_present_flag ? 1 : 0;
        p->constrained_intra_pred_flag =
            pps_.constrained_intra_pred_flag ? 1 : 0;
        p->pic_init_qp_minus26 =
            static_cast<CHAR>(pps_.pic_init_qp_minus26);
        p->pic_init_qs_minus26 =
            static_cast<CHAR>(pps_.pic_init_qs_minus26);
        p->chroma_qp_index_offset =
            static_cast<CHAR>(pps_.chroma_qp_index_offset);
        p->second_chroma_qp_index_offset =
            static_cast<CHAR>(pps_.chroma_qp_index_offset);
        p->num_ref_idx_l0_active_minus1 =
            static_cast<UCHAR>(pps_.num_ref_idx_l0_default_active_minus1);
        p->num_ref_idx_l1_active_minus1 =
            static_cast<UCHAR>(pps_.num_ref_idx_l1_default_active_minus1);
        p->IntraPicFlag = is_idr ? 1 : 0;
        p->MinLumaBipredSize8x8Flag =
            (sps_.level_idc >= 31) ? 1 : 0;

        // ContinuationFlag MUST be 1 to signal the driver that the
        // back half of DXVA_PicParams_H264 (CurrPic.AssociatedFlag
        // through MinLumaBipredSize8x8Flag) is filled. Without it,
        // NVIDIA/Intel drivers treat the struct as "header only",
        // which is why SubmitDecoderBuffers returns SUCCESS but
        // the NV12 surface remains zero-filled (same symptom as
        // Intel iHD's IQMatrix quirk on Linux, different root
        // cause).
        p->ContinuationFlag = 1;

        hr = video_ctx_->ReleaseDecoderBuffer(
            decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS);
        return SUCCEEDED(hr);
    }

    bool SubmitQuantizationMatrix() {
        UINT size = 0;
        void* mapped = nullptr;
        HRESULT hr = video_ctx_->GetDecoderBuffer(
            decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
            &size, &mapped);
        if (FAILED(hr) || !mapped
            || size < sizeof(DXVA_Qmatrix_H264)) {
            return false;
        }
        // No custom scaling lists in our streams — fill with 16
        // (ITU default "no scaling"). Every driver we care about
        // treats all-16s as a no-op.
        auto* q = static_cast<DXVA_Qmatrix_H264*>(mapped);
        std::memset(q, 16, sizeof(*q));
        hr = video_ctx_->ReleaseDecoderBuffer(
            decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX);
        return SUCCEEDED(hr);
    }

    // DXVA2 H.264 long-format bitstream: the buffer must contain
    // the 3-byte Annex-B start code (00 00 01) in front of the NAL,
    // with BSNALunitDataLocation pointing at the start code and
    // SliceBytesInBuffer covering prefix + NAL. NVIDIA silently
    // returns SUCCESS on both variants but only writes real pixels
    // when the start code is present — the same class of bug as
    // Intel iHD's IQMatrix requirement on Linux, different surface.
    // Buffer is zero-padded up to a 128-byte multiple; some
    // drivers (including NVIDIA's) require that alignment.
    bool SubmitBitstream(const std::uint8_t* nal_full,
                          std::size_t nal_full_len) {
        UINT size = 0;
        void* mapped = nullptr;
        HRESULT hr = video_ctx_->GetDecoderBuffer(
            decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &size, &mapped);
        const std::size_t payload = 3 + nal_full_len;
        const std::size_t padded = (payload + 127) & ~std::size_t{127};
        if (FAILED(hr) || !mapped || size < padded) {
            return false;
        }
        auto* dst = static_cast<std::uint8_t*>(mapped);
        dst[0] = 0; dst[1] = 0; dst[2] = 1;
        std::memcpy(dst + 3, nal_full, nal_full_len);
        if (padded > payload) {
            std::memset(dst + payload, 0, padded - payload);
        }
        last_bitstream_size_ = static_cast<UINT>(padded);
        last_slice_bytes_ = static_cast<UINT>(payload);
        hr = video_ctx_->ReleaseDecoderBuffer(
            decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
        return SUCCEEDED(hr);
    }

    bool SubmitSliceControl(std::size_t /*nal_full_len*/,
                              const ParsedSliceHeader& sh) {
        UINT size = 0;
        void* mapped = nullptr;
        HRESULT hr = video_ctx_->GetDecoderBuffer(
            decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
            &size, &mapped);
        if (FAILED(hr) || !mapped
            || size < sizeof(DXVA_Slice_H264_Short)) {
            return false;
        }
        auto* s = static_cast<DXVA_Slice_H264_Short*>(mapped);
        s->BSNALunitDataLocation = 0;  // points at the 00 00 01
        // SliceBytesInBuffer must cover the full padded bitstream
        // (128-byte aligned), not just the start-code + NAL. FFmpeg
        // extends the last slice's byte count by the trailing
        // padding; NVIDIA's DXVA2 driver silently writes zeros when
        // SliceBytesInBuffer doesn't match the bitstream buffer's
        // padded length.
        s->SliceBytesInBuffer = last_bitstream_size_;
        s->wBadSliceChopping = 0;
        (void)sh;
        hr = video_ctx_->ReleaseDecoderBuffer(
            decoder_.Get(),
            D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);
        return SUCCEEDED(hr);
    }

    void DestroyContext() {
        decoder_.Reset();
        for (auto& v : views_) v.Reset();
        for (auto& t : textures_) t.Reset();
        for (auto& t : shader_textures_) t.Reset();
        prev_ref_idx_ = -1;
        have_context_ = false;
    }

    Config cfg_{};
    FrameReady on_frame_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_ctx_;
    ComPtr<ID3D11VideoDecoder> decoder_;
    ComPtr<ID3D11Texture2D> textures_[kSurfaceCount];
    ComPtr<ID3D11Texture2D> shader_textures_[kSurfaceCount];
    ComPtr<ID3D11VideoDecoderOutputView> views_[kSurfaceCount];

    bool have_sps_ = false;
    bool have_pps_ = false;
    bool have_context_ = false;
    int cfg_bitstream_raw_ = 1;
    UINT last_bitstream_size_ = 0;
    UINT last_slice_bytes_ = 0;
    ParsedSps sps_{};
    ParsedPps pps_{};
    int width_ = 0;
    int height_ = 0;
    int prev_ref_idx_ = -1;
    int prev_frame_num_ = 0;
    std::uint64_t frame_count_ = 0;
    std::uint64_t skipped_nonidr_ = 0;
};

}  // namespace

std::unique_ptr<Decoder> MakeD3d11VaDecoder() {
    return std::make_unique<D3d11VaDecoder>();
}

}  // namespace unio

#endif  // _WIN32
