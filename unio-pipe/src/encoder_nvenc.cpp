// NVENC H.264 encoder — Windows sibling of encoder_vaapi.cpp.
// Consumes the BGRA CpuFrame the WGC capture staging-texture
// map produces, emits Annex-B H.264 NAL bytes.
//
// NVENC runtime lives in nvEncodeAPI64.dll, which ships with
// every NVIDIA driver (this TU LoadLibrarys it rather than
// linking at build time, so the build itself has no NVIDIA
// dependency). The function pointer table comes out of
// NvEncodeAPICreateInstance.
//
// Scope matches Day 8c: Main-profile H.264 for lowest common
// decode denominator, CQP rate control (matches the VA-API side),
// in-sysmem BGRA input buffer (simple — zero-copy via
// NvEncRegisterResource on a shared ID3D11Texture2D is the PR 9
// optimisation once we unify the D3D11 device across capture
// and encode).

#if !defined(_WIN32)
#include "encoder.h"
namespace unio {
std::unique_ptr<Encoder> MakeNvencEncoder() { return nullptr; }
}  // namespace unio
#else

#include "encoder.h"
#include "h264_parse.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <d3d11.h>
#include <wrl/client.h>

#include <ffnvcodec/nvEncodeAPI.h>

#pragma comment(lib, "d3d11.lib")

using Microsoft::WRL::ComPtr;

namespace unio {

namespace {

std::uint64_t NowNs() {
    // system_clock so cross-machine capture/decode/present rows
    // are directly comparable on NTP-synced hosts.
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count());
}

class NvencEncoder final : public Encoder {
public:
    NvencEncoder() = default;
    ~NvencEncoder() override { Teardown(); }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;
        if (auto err = LoadDll(); err) return err;

        // Dedicated D3D11 device for NVENC — picks the NVIDIA
        // adapter when available. D3D11CreateDevice with default
        // HARDWARE type lands on whichever adapter Windows
        // prefers, which on hybrid GPU systems is usually the
        // integrated one. Enumerate adapters and pick NVIDIA
        // explicitly so NVENC succeeds.
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return "CreateDXGIFactory1 failed";
        }
        ComPtr<IDXGIAdapter1> chosen;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.VendorId == 0x10DE) {   // NVIDIA
                chosen = a;
                break;
            }
        }
        if (!chosen) {
            return "no NVIDIA GPU found (vendor 0x10DE)";
        }

        UINT flags = 0;
        D3D_FEATURE_LEVEL fl = {};
        if (FAILED(D3D11CreateDevice(
                chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                flags, nullptr, 0, D3D11_SDK_VERSION,
                &device_, &fl, &ctx_))) {
            return "D3D11CreateDevice on NVIDIA adapter failed";
        }

        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{};
        open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        open.device = device_.Get();
        open.apiVersion = NVENCAPI_VERSION;
        if (nv_.nvEncOpenEncodeSessionEx(&open, &session_)
            != NV_ENC_SUCCESS || !session_) {
            return "nvEncOpenEncodeSessionEx failed";
        }

        // Preset: P4 + LOW_LATENCY. PR 7 Day 1 evaluated both
        // P1 + ULTRA_LOW_LATENCY (2× worse — P1 emits ~2× larger
        // P-frames and the bitstream cost dominated the encode-
        // time savings) and the orthogonal rcParams knobs
        // (lookaheadDepth=0, enableAQ=0, enableIntraRefresh=0),
        // which crashed NVENC on init. Day 1 reverted; the real
        // Windows-source latency win is PR 7 Day 2's zero-copy
        // WGC→NvEncRegisterResource path, not the preset tune.
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
        ec.rcParams.constQP.qpIntra = cfg.quality;
        ec.rcParams.constQP.qpInterP = cfg.quality;
        ec.rcParams.constQP.qpInterB = cfg.quality;
        // One IDR up front; the control plane drives keyframes
        // via request_idr, same as the VA-API side. No periodic
        // intra refresh in the MVP.
        ec.gopLength = NVENC_INFINITE_GOPLENGTH;
        ec.frameIntervalP = 1;  // IPPP
        ec.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        ec.encodeCodecConfig.h264Config.repeatSPSPPS = 1;  // on every IDR
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
        ip.enablePTD = 1;       // NVENC picks frame types
        ip.encodeConfig = &ec;
        if (nv_.nvEncInitializeEncoder(session_, &ip) != NV_ENC_SUCCESS) {
            return "nvEncInitializeEncoder failed";
        }

        NV_ENC_CREATE_INPUT_BUFFER ib{};
        ib.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
        ib.width = cfg.width;
        ib.height = cfg.height;
        ib.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;  // BGRA
        if (nv_.nvEncCreateInputBuffer(session_, &ib) != NV_ENC_SUCCESS) {
            return "nvEncCreateInputBuffer failed";
        }
        input_buffer_ = ib.inputBuffer;

        NV_ENC_CREATE_BITSTREAM_BUFFER bb{};
        bb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (nv_.nvEncCreateBitstreamBuffer(session_, &bb) != NV_ENC_SUCCESS) {
            return "nvEncCreateBitstreamBuffer failed";
        }
        bitstream_buffer_ = bb.bitstreamBuffer;

        std::fprintf(stderr,
            "unio-pipe: NVENC initialized %dx%d@%dfps, "
            "H.264 Main, CQP %d\n",
            cfg.width, cfg.height, cfg.fps, cfg.quality);
        initialized_ = true;
        return std::nullopt;
    }

    void ForceIdr() override {
        force_idr_.store(true, std::memory_order_release);
    }

    EncodedPacketPtr Encode(const CpuFrame& frame) override {
        if (!initialized_) return nullptr;

        // Upload BGRA to NVENC's input buffer. The lock/unlock
        // pair is NVENC's idiom for CPU-to-GPU transfer on the
        // SYSMEM_CACHED heap — the driver handles the copy.
        NV_ENC_LOCK_INPUT_BUFFER lock{};
        lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lock.inputBuffer = input_buffer_;
        if (nv_.nvEncLockInputBuffer(session_, &lock) != NV_ENC_SUCCESS) {
            return nullptr;
        }
        const std::uint32_t rows = std::min<std::uint32_t>(
            frame.height, static_cast<std::uint32_t>(cfg_.height));
        const std::uint32_t src_stride = frame.stride_bytes;
        const std::uint32_t dst_stride = lock.pitch;
        const std::uint32_t row_bytes = std::min<std::uint32_t>(
            src_stride, dst_stride);
        auto* dst = static_cast<std::uint8_t*>(lock.bufferDataPtr);
        const auto* src = frame.pixels.data();
        for (std::uint32_t y = 0; y < rows; ++y) {
            std::memcpy(dst + y * dst_stride,
                        src + y * src_stride, row_bytes);
        }
        nv_.nvEncUnlockInputBuffer(session_, input_buffer_);

        return EncodeMappedInput(input_buffer_,
                                 static_cast<std::uint32_t>(cfg_.width) * 4,
                                 frame.frame_id,
                                 frame.capture_monotonic_ns);
    }

    bool AcceptsGpu() const override { return true; }

    void* NativeD3d11Device() const override {
        return device_.Get();
    }

    EncodedPacketPtr EncodeGpu(const GpuFrame& frame) override {
        if (!initialized_ || !frame.native_texture) return nullptr;
        auto* tex = static_cast<ID3D11Texture2D*>(frame.native_texture);

        // NvEncRegisterResource is expensive — do it once per unique
        // texture pointer and cache. The capture side owns a small
        // pool (~2 slots) so this LRU cache never grows past that.
        NV_ENC_REGISTERED_PTR reg = LookupOrRegister(tex);
        if (!reg) {
            std::fprintf(stderr,
                "unio-pipe: NvEncRegisterResource failed\n");
            return nullptr;
        }
        NV_ENC_MAP_INPUT_RESOURCE map_req{};
        map_req.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map_req.registeredResource = reg;
        if (nv_.nvEncMapInputResource(session_, &map_req)
                != NV_ENC_SUCCESS) {
            return nullptr;
        }
        auto pkt = EncodeMappedInput(
            map_req.mappedResource,
            /*pitch=*/static_cast<std::uint32_t>(cfg_.width) * 4,
            frame.frame_id, frame.capture_monotonic_ns);
        nv_.nvEncUnmapInputResource(session_, map_req.mappedResource);
        return pkt;
    }

private:
    NV_ENC_REGISTERED_PTR LookupOrRegister(ID3D11Texture2D* tex) {
        for (auto& e : reg_cache_) {
            if (e.tex == tex) return e.reg;
        }
        NV_ENC_REGISTER_RESOURCE rr{};
        rr.version = NV_ENC_REGISTER_RESOURCE_VER;
        rr.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
        rr.width = static_cast<std::uint32_t>(cfg_.width);
        rr.height = static_cast<std::uint32_t>(cfg_.height);
        rr.pitch = 0;  // driver derives from the texture desc
        rr.resourceToRegister = tex;
        rr.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
        rr.bufferUsage = NV_ENC_INPUT_IMAGE;
        if (nv_.nvEncRegisterResource(session_, &rr) != NV_ENC_SUCCESS) {
            return nullptr;
        }
        reg_cache_.push_back({tex, rr.registeredResource});
        return rr.registeredResource;
    }

    EncodedPacketPtr EncodeMappedInput(NV_ENC_INPUT_PTR input,
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
                "unio-pipe: nvenc encode frame %llu idr=%d flags=0x%x\n",
                static_cast<unsigned long long>(frame_count_),
                is_idr ? 1 : 0,
                static_cast<unsigned>(pp.encodePicFlags));
            std::fflush(stderr);
        }
        auto st = nv_.nvEncEncodePicture(session_, &pp);
        if (st == NV_ENC_ERR_NEED_MORE_INPUT) {
            std::fprintf(stderr,
                "unio-pipe: nvenc need_more_input on frame %llu\n",
                static_cast<unsigned long long>(frame_count_));
            ++frame_count_;
            auto skip = std::make_unique<EncodedPacket>();
            skip->frame_id = frame_id;
            skip->capture_monotonic_ns = capture_ns;
            return skip;
        }
        if (st != NV_ENC_SUCCESS) {
            std::fprintf(stderr,
                "unio-pipe: nvenc encode failed: %d\n",
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
        // Latency SEI ahead of the NVENC-emitted bitstream. SEI is
        // valid anywhere before its target VCL NAL, so prepending
        // is always safe regardless of NVENC's SPS/PPS cadence.
        auto sei = BuildLatencySeiAnnexB(
            pkt->frame_id, pkt->capture_monotonic_ns);
        pkt->nal_bytes.reserve(sei.size() + lb.bitstreamSizeInBytes);
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                              sei.begin(), sei.end());
        const auto* bs = static_cast<const std::uint8_t*>(
            lb.bitstreamBufferPtr);
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                              bs, bs + lb.bitstreamSizeInBytes);
        nv_.nvEncUnlockBitstream(session_, bitstream_buffer_);
        if (log_n < 5 && !pkt->nal_bytes.empty()) {
            const std::uint8_t nal_hdr =
                pkt->nal_bytes.size() > 4 ? pkt->nal_bytes[4] : 0;
            std::fprintf(stderr,
                "unio-pipe: nvenc emitted %u bytes, "
                "pic_type=%d, first nal type=%u\n",
                static_cast<unsigned>(lb.bitstreamSizeInBytes),
                static_cast<int>(lb.pictureType),
                static_cast<unsigned>(nal_hdr & 0x1F));
        }
        pkt->encode_done_monotonic_ns = NowNs();
        ++frame_count_;
        return pkt;
    }

public:
    std::string_view Name() const override { return "nvenc"; }

private:
    std::optional<std::string> LoadDll() {
        dll_ = ::LoadLibraryA("nvEncodeAPI64.dll");
        if (!dll_) {
            return "nvEncodeAPI64.dll not found (install NVIDIA driver)";
        }
        using PFN_CREATE =
            NVENCSTATUS (NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST*);
        auto create = reinterpret_cast<PFN_CREATE>(
            ::GetProcAddress(dll_, "NvEncodeAPICreateInstance"));
        if (!create) {
            return "NvEncodeAPICreateInstance not in nvEncodeAPI64.dll";
        }
        nv_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (create(&nv_) != NV_ENC_SUCCESS) {
            return "NvEncodeAPICreateInstance failed (driver too old?)";
        }
        return std::nullopt;
    }

    void Teardown() {
        if (session_) {
            for (auto& e : reg_cache_) {
                if (e.reg) nv_.nvEncUnregisterResource(session_, e.reg);
            }
            reg_cache_.clear();
            if (input_buffer_) {
                nv_.nvEncDestroyInputBuffer(session_, input_buffer_);
                input_buffer_ = nullptr;
            }
            if (bitstream_buffer_) {
                nv_.nvEncDestroyBitstreamBuffer(session_,
                                                  bitstream_buffer_);
                bitstream_buffer_ = nullptr;
            }
            nv_.nvEncDestroyEncoder(session_);
            session_ = nullptr;
        }
        ctx_.Reset();
        device_.Reset();
        // Intentionally do NOT FreeLibrary(dll_). NVENC spawns
        // internal worker threads and registers driver callbacks
        // that can fire after nvEncDestroyEncoder returns; tearing
        // down the DLL on stop-restart cycles races those threads
        // and lands as a wild jump into freed code pages.
        // nvEncodeAPI64.dll is ~2 MB; leaking the load across the
        // helper's lifetime is free and the crash is deterministic
        // without it. (Saw offset 0x136f26 on the 2nd restart cycle.)
        initialized_ = false;
    }

    struct RegistryEntry {
        ID3D11Texture2D* tex;
        NV_ENC_REGISTERED_PTR reg;
    };

    Config cfg_{};
    HMODULE dll_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST nv_{};
    void* session_ = nullptr;
    NV_ENC_INPUT_PTR input_buffer_ = nullptr;
    NV_ENC_OUTPUT_PTR bitstream_buffer_ = nullptr;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    bool initialized_ = false;
    std::atomic<bool> force_idr_{true};
    std::uint64_t frame_count_ = 0;
    // NvEncRegisterResource results, keyed by the raw texture
    // pointer. WGC capture owns 2 pool slots so this never grows
    // past that. Unregistered in Teardown() before the session is
    // destroyed — the NVENC SDK requires registered resources to
    // be unregistered explicitly while the session is still alive.
    std::vector<RegistryEntry> reg_cache_;
};

}  // namespace

std::unique_ptr<Encoder> MakeNvencEncoder() {
    return std::make_unique<NvencEncoder>();
}

}  // namespace unio

#endif  // _WIN32
