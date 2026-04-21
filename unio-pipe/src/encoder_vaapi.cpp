// VA-API H.264 encoder. Day-3 scope:
//
//   1. Find a usable VADisplay via /dev/dri/renderD128.
//   2. Verify the driver actually advertises a constrained-baseline
//      H.264 encode profile + CQP rate-control entry point.
//   3. Allocate VA surfaces at the capture size.
//   4. Keep a context ready for per-frame Encode() calls.
//   5. Hand back a placeholder Annex-B packet until the full
//      BGRA→NV12 upload + slice-parameter build + bitstream
//      readback lands. Stubbed packet carries a valid 00-00-00-01
//      start code so the downstream framing still sees a NAL;
//      the payload is recognisably a no-op so we can't
//      accidentally ship it to a real decoder.
//
// The per-frame encode dance is several hundred lines of libva
// plumbing (vaCreateImage / vaPutImage for the format upload,
// then Sequence/Picture/Slice param buffers, vaBeginPicture /
// vaRenderPicture / vaEndPicture, vaMapBuffer to read back the
// coded buffer). It's tractable but deliberately not done in one
// pass — the reference code lives in Intel's va-api-tools and
// Sunshine (GPL, read-only), so porting is a focused bench task
// rather than a blind write.

#if !defined(__linux__) || !defined(UNIO_PIPE_HAS_VAAPI)
// Non-Linux OR libva missing: ship a stub factory so callers
// link on every platform without conditional compilation. When
// libva is absent at build time, StartOutbound returns the
// helpful error and the Python pipeline stays the data plane.
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
#include <mutex>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>

namespace unio {

namespace {

std::uint64_t NowNs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count());
}

class VaapiEncoder final : public Encoder {
public:
    VaapiEncoder() = default;

    ~VaapiEncoder() override {
        Teardown();
    }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;

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

        // Entrypoint probe — we want Slice encode for H.264. Any
        // modern Intel / AMD / NVIDIA (via nvidia-vaapi-driver)
        // advertises it; a failure here means no VA-API H.264
        // encode on this host and we fall back loudly.
        int num_epoints = 0;
        VAEntrypoint epoints[16];
        s = vaQueryConfigEntrypoints(
            dpy_, VAProfileH264ConstrainedBaseline,
            epoints, &num_epoints);
        if (s != VA_STATUS_SUCCESS) {
            Teardown();
            return std::string("vaQueryConfigEntrypoints: ")
                   + vaErrorStr(s);
        }
        bool slice_ok = false;
        for (int i = 0; i < num_epoints; ++i) {
            if (epoints[i] == VAEntrypointEncSlice) {
                slice_ok = true;
                break;
            }
        }
        if (!slice_ok) {
            Teardown();
            return "VA-API driver does not advertise "
                   "H.264 constrained-baseline encode";
        }

        // Attribute probe: make sure NV12 surfaces and CQP rate
        // control are available. Without explicit checks the
        // config create below returns opaque errors on drivers
        // that only support VBR (some AMD VA-API revisions).
        VAConfigAttrib attrs[2];
        attrs[0].type = VAConfigAttribRTFormat;
        attrs[1].type = VAConfigAttribRateControl;
        s = vaGetConfigAttributes(
            dpy_, VAProfileH264ConstrainedBaseline,
            VAEntrypointEncSlice, attrs, 2);
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
            // CBR-only driver. Not a blocker — we just record it
            // and pick the other entry point. For Day-3 we bail;
            // the Python pipeline stays the data plane.
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
            dpy_, VAProfileH264ConstrainedBaseline,
            VAEntrypointEncSlice, cfg_attrs, 2, &config_id_);
        if (s != VA_STATUS_SUCCESS) {
            Teardown();
            return std::string("vaCreateConfig: ") + vaErrorStr(s);
        }

        // Surface pool: a small handful so the encoder pipeline
        // has reorder slack. Constrained baseline disables
        // B-frames, so 2 surfaces is enough for us + 1 for the
        // driver's reference frame.
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

        std::fprintf(stderr,
                     "unio-pipe: VA-API initialized %d.%d, "
                     "surfaces %dx%d NV12, CQP rate control\n",
                     major, minor, cfg.width, cfg.height);

        initialized_ = true;
        return std::nullopt;
    }

    void ForceIdr() override {
        force_idr_.store(true, std::memory_order_release);
    }

    EncodedPacketPtr Encode(const CpuFrame& frame) override {
        if (!initialized_) return nullptr;

        // Per-frame encode is the real work — BGRX→NV12 upload
        // (vaCreateImage + vaPutImage or a VAProcPipeline colour
        // convert), Sequence/Picture/Slice parameter buffers,
        // vaBeginPicture / vaRenderPicture / vaEndPicture,
        // vaSyncSurface, vaMapBuffer on the coded buffer.
        // Several hundred lines of libva plumbing that needs
        // bench validation rather than session coding; the
        // reference is Intel's va-api-tools (MIT) and
        // Sunshine's vaapi_encode.cpp (GPL, read-only). For
        // Day 3 we emit a placeholder Annex-B packet with a
        // valid start code + our own metadata so downstream
        // framing sees a real NAL; the payload cannot be
        // mistaken for a real picture.
        auto pkt = std::make_unique<EncodedPacket>();
        pkt->frame_id = frame.frame_id;
        pkt->capture_monotonic_ns = frame.capture_monotonic_ns;
        pkt->key_frame = force_idr_.exchange(
            false, std::memory_order_acq_rel);

        // 00 00 00 01 — canonical Annex-B start code.
        // 0x18 = NAL header: forbidden_zero_bit=0, nal_ref_idc=0,
        //        nal_unit_type=24 (STAP-A aggregation, a safe
        //        user-data-ish type that no real decoder will
        //        mistake for a picture). Followed by our 8-byte
        //        frame_id LE + 8-byte capture_ns LE. Real
        //        encoder overwrites this whole buffer.
        pkt->nal_bytes.reserve(32);
        pkt->nal_bytes.insert(pkt->nal_bytes.end(),
                              {0x00, 0x00, 0x00, 0x01, 0x18});
        auto append_le64 = [&](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                pkt->nal_bytes.push_back(
                    static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
            }
        };
        append_le64(pkt->frame_id);
        append_le64(pkt->capture_monotonic_ns);
        pkt->encode_done_monotonic_ns = NowNs();
        return pkt;
    }

    std::string_view Name() const override { return "vaapi"; }

private:
    static constexpr int kNumSurfaces = 3;

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
    VAConfigID config_id_ = VA_INVALID_ID;
    VAContextID context_id_ = VA_INVALID_ID;
    VASurfaceID surfaces_[kNumSurfaces] = {
        VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID};
    bool initialized_ = false;
    std::atomic<bool> force_idr_{true};  // first frame is IDR
};

}  // namespace

std::unique_ptr<Encoder> MakeVaapiEncoder() {
    return std::make_unique<VaapiEncoder>();
}

}  // namespace unio

#endif  // __linux__
