// Windows Graphics Capture. Windows sibling of
// capture_xcomposite.cpp on the source side.
//
//   GraphicsCaptureItem::CreateFromMonitorInterop(HMONITOR)
//        │
//   Direct3D11CaptureFramePool::CreateFreeThreaded(
//        D3D11Device,  B8G8R8A8, frame_count=2, size)
//        │
//   pool.FrameArrived  ──►  pool.TryGetNextFrame()
//        │                         │
//        │         IDirect3DSurface → IDirect3DDxgiInterfaceAccess
//        │                         │
//        │                  ID3D11Texture2D (GPU, BGRA)
//        │                         │
//        │          CopyResource → staging (CPU_READ) → Map
//        │                         │
//        └──────────────►   BGRX CpuFrame → callback
//
// CPU-readback path for the MVP — the existing Encoder interface
// wants a CpuFrame. Once NVENC lands (Day 8c), a follow-up can
// register the WGC ID3D11Texture2D with NVENC directly and skip
// the readback entirely (zero-copy GPU→NVENC).

#if defined(_WIN32)

#include "capture_wgc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>
#include <inspectable.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowsapp.lib")

using Microsoft::WRL::ComPtr;
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgd11 = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace unio {

namespace {

std::uint64_t NowNs() {
    // system_clock so cross-machine capture/decode/present rows
    // are directly comparable on NTP-synced hosts.
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        system_clock::now().time_since_epoch()).count());
}

// One-time QPC ↔ system_clock calibration so we can convert
// Direct3D11CaptureFrame::SystemRelativeTime (a boot-relative
// 100-ns tick count) into a Unix-epoch nanosecond timestamp
// comparable with our other stages. WGC's SRT is the compositor
// vblank time the frame was captured — 1–5 ms earlier than our
// OnFrame dispatch. Using it as capture_ns removes that bias
// and moves our "capture" timestamp to the true beginning of
// the user-facing pipeline.
struct SrtCalibration {
    std::int64_t qpc_ticks_at_cal = 0;
    std::int64_t qpc_frequency = 10000000;  // fallback: 100ns ticks
    std::uint64_t system_ns_at_cal = 0;
};

SrtCalibration CalibrateSrt() {
    SrtCalibration c;
    LARGE_INTEGER freq{};
    LARGE_INTEGER qpc{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&qpc);
    c.qpc_ticks_at_cal = qpc.QuadPart;
    c.qpc_frequency = freq.QuadPart;
    c.system_ns_at_cal = NowNs();
    return c;
}

std::uint64_t SrtToSystemNs(
        const winrt::Windows::Foundation::TimeSpan& srt,
        const SrtCalibration& cal) {
    // SRT is a TimeSpan (100-ns units) relative to system boot,
    // matching QPC's timebase in the abstract "time since boot"
    // sense but in 100-ns units regardless of QPC frequency.
    // Convert cal's QPC reading to 100-ns units, then delta.
    const std::int64_t cal_hundredns =
        (cal.qpc_ticks_at_cal * 10000000) / cal.qpc_frequency;
    const std::int64_t delta_hundredns =
        srt.count() - cal_hundredns;
    // Negative delta means the frame was composited before our
    // calibration (possible on the very first frame when WGC
    // had pre-init pixels queued). Clamp to 0 offset rather
    // than underflow the unsigned return.
    if (delta_hundredns < 0 &&
        static_cast<std::uint64_t>(-delta_hundredns) * 100
            > cal.system_ns_at_cal) {
        return cal.system_ns_at_cal;
    }
    return static_cast<std::uint64_t>(
        static_cast<std::int64_t>(cal.system_ns_at_cal)
        + delta_hundredns * 100);
}

wgd11::IDirect3DDevice CreateWinrtDevice(ID3D11Device* dev) {
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
        return nullptr;
    }
    winrt::com_ptr<::IInspectable> insp;
    if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(
            dxgi.Get(),
            reinterpret_cast<::IInspectable**>(
                winrt::put_abi(insp))))) {
        return nullptr;
    }
    return insp.as<wgd11::IDirect3DDevice>();
}

ComPtr<ID3D11Texture2D> TextureFromSurface(
        const wgd11::IDirect3DSurface& surface) {
    auto access = surface.as<
        Windows::Graphics::DirectX::Direct3D11::
            IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> tex;
    access->GetInterface(IID_PPV_ARGS(&tex));
    return tex;
}

}  // namespace

struct WgcCapture::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Texture2D> staging;
    wgd11::IDirect3DDevice winrt_device{nullptr};

    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token frame_token{};

    std::atomic<bool> running{false};
    std::uint64_t frame_id = 0;
    WgcCapture::FrameCallback cb = nullptr;
    WgcCapture::GpuFrameCallback gpu_cb = nullptr;
    void* user = nullptr;
    int width = 0;
    int height = 0;
    bool com_inited = false;
    SrtCalibration srt_cal{};
    // Serialises OnFrame (WinRT worker thread) against Close
    // (main thread). Without it, CopyResource / Map inside OnFrame
    // crashes in d3d11.dll when Close resets the device mid-call —
    // the "unio-pipe.exe stopped working" dialog on stream stop.
    std::mutex frame_mu;

    // PR 7 Day 2: owned GPU-resident texture pool for the zero-
    // copy path. FrameArrived → GPU-to-GPU CopyResource into one
    // of these → hand the pointer to NVENC. 2 slots = one for the
    // in-flight encode, one for the next capture.
    static constexpr int kGpuPoolCount = 2;
    ComPtr<ID3D11Texture2D> gpu_pool[kGpuPoolCount];
    int gpu_next_idx = 0;

    // Called from a msquic / WGC worker thread on every arrival.
    // Must not block the thread for long — CopyResource + Map +
    // memcpy is ~3-5 ms at 1080p, well under the 16 ms frame
    // budget.
    void OnFrame() {
        std::lock_guard<std::mutex> lk(frame_mu);
        if (!running.load(std::memory_order_acquire)) return;
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        auto surface = frame.Surface();
        auto tex = TextureFromSurface(surface);
        if (!tex) return;

        // PR 9.1 Part A1: use the compositor vblank time, not
        // our OnFrame dispatch time. SRT is 1–5 ms earlier.
        const std::uint64_t capture_ns =
            SrtToSystemNs(frame.SystemRelativeTime(), srt_cal);

        if (gpu_cb) {
            // Zero-copy path: GPU-to-GPU CopyResource into our own
            // pool slot, release the WGC frame, hand the pointer
            // to NVENC. The encoder registers the texture once per
            // slot and maps it per frame — ~0.1 ms all-in on Diana
            // vs ~5 ms for the CPU staging-Map + memcpy.
            const int idx = gpu_next_idx;
            gpu_next_idx = (gpu_next_idx + 1) % kGpuPoolCount;
            ctx->CopyResource(gpu_pool[idx].Get(), tex.Get());
            GpuFrame gf{};
            gf.native_texture = gpu_pool[idx].Get();
            gf.width  = static_cast<std::uint32_t>(width);
            gf.height = static_cast<std::uint32_t>(height);
            gf.frame_id = ++frame_id;
            gf.capture_monotonic_ns = capture_ns;
            gpu_cb(gf, user);
            return;
        }

        // CPU path (Linux parity / fallback): staging-Map + memcpy
        // into a CpuFrame. Retained for Linux sink compatibility
        // and for Windows encoders that don't expose a D3D11 device.
        ctx->CopyResource(staging.Get(), tex.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0,
                            D3D11_MAP_READ, 0, &m))) return;

        auto cf = std::make_unique<CpuFrame>();
        cf->width  = static_cast<std::uint32_t>(width);
        cf->height = static_cast<std::uint32_t>(height);
        cf->stride_bytes =
            static_cast<std::uint32_t>(width) * 4;
        cf->frame_id = ++frame_id;
        cf->capture_monotonic_ns = capture_ns;
        cf->pixels.resize(
            static_cast<std::size_t>(cf->stride_bytes) * cf->height);
        const auto* src = static_cast<const std::uint8_t*>(m.pData);
        std::uint8_t* dst = cf->pixels.data();
        for (std::uint32_t y = 0; y < cf->height; ++y) {
            std::memcpy(dst + y * cf->stride_bytes,
                        src + y * m.RowPitch,
                        cf->stride_bytes);
        }
        ctx->Unmap(staging.Get(), 0);

        if (cb) cb(std::move(cf), user);
    }
};

WgcCapture::WgcCapture() : impl_(std::make_shared<Impl>()) {}
WgcCapture::~WgcCapture() { Close(); }

bool WgcCapture::Open(void* shared_device) {
    HRESULT hr = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        impl_->com_inited = (hr == S_OK);
    } else {
        return false;
    }

    if (shared_device) {
        // PR 7 Day 2: adopt the encoder's D3D11 device so WGC
        // captures into textures NVENC can directly register.
        impl_->device = static_cast<ID3D11Device*>(shared_device);
        impl_->device->GetImmediateContext(&impl_->ctx);
    } else {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL got = {};
        D3D_FEATURE_LEVEL wanted[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        if (FAILED(D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                wanted, 2, D3D11_SDK_VERSION,
                &impl_->device, &got, &impl_->ctx))) {
            return false;
        }
    }

    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(impl_->device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    impl_->winrt_device = CreateWinrtDevice(impl_->device.Get());
    if (!impl_->winrt_device) return false;

    if (!wgc::GraphicsCaptureSession::IsSupported()) {
        std::fprintf(stderr,
            "unio-pipe: WGC not supported on this OS build\n");
        return false;
    }

    // PR 9.1 Part A1: calibrate the QPC↔system_clock offset once
    // per helper session. Both are monotonic hardware counters
    // on modern Windows so no drift correction is needed at our
    // timescales.
    impl_->srt_cal = CalibrateSrt();
    return true;
}

bool WgcCapture::SetupMonitor(Impl& impl, WgcRect rect) {
    impl.width = rect.width;
    impl.height = rect.height;
    HMONITOR hmon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    auto factory = winrt::get_activation_factory<
        wgc::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();
    HRESULT hr = factory->CreateForMonitor(
        hmon, winrt::guid_of<wgc::GraphicsCaptureItem>(),
        winrt::put_abi(impl.item));
    if (FAILED(hr) || !impl.item) {
        std::fprintf(stderr,
            "unio-pipe: WGC CreateForMonitor failed HR=0x%x\n",
            static_cast<unsigned>(hr));
        return false;
    }
    auto item_size = impl.item.Size();
    if (impl.width  == 0) impl.width  = item_size.Width;
    if (impl.height == 0) impl.height = item_size.Height;
    return true;
}

void WgcCapture::StartSession(const std::shared_ptr<Impl>& impl) {
    impl->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        impl->winrt_device,
        wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2, {impl->width, impl->height});
    impl->running.store(true, std::memory_order_release);
    std::weak_ptr<WgcCapture::Impl> weak(impl);
    impl->frame_token = impl->pool.FrameArrived(
        [weak](wgc::Direct3D11CaptureFramePool const&,
               winrt::Windows::Foundation::IInspectable const&) {
            if (auto p = weak.lock()) p->OnFrame();
        });
    impl->session = impl->pool.CreateCaptureSession(impl->item);
    try {
        impl->session.IsCursorCaptureEnabled(true);
    } catch (...) {}
    // Hide the yellow WGC capture border via
    // GraphicsCaptureSession::IsBorderRequired(false). Added in
    // Windows 11 22000. Our CI builds against the Win10 10.0.19041
    // SDK which doesn't expose the method, so it's compiled out
    // here. To enable on a Win11 build: ensure the Windows SDK
    // target is ≥ 10.0.22000.0, then guard with
    //   if (ApiInformation::IsPropertyPresent(L"Windows.Graphics.
    //       Capture.GraphicsCaptureSession", L"IsBorderRequired"))
    //     impl->session.IsBorderRequired(false);
    // so Win10 runtime still falls through cleanly. No API exists
    // to hide the yellow border on Win10 22H2; users on that
    // build will continue to see it.
    impl->session.StartCapture();
}

bool WgcCapture::Start(WgcRect rect, int /*fps*/,
                        FrameCallback cb, void* user) {
    impl_->cb = cb;
    impl_->user = user;
    if (!SetupMonitor(*impl_, rect)) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(impl_->width);
    td.Height = static_cast<UINT>(impl_->height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(impl_->device->CreateTexture2D(
            &td, nullptr, &impl_->staging))) return false;

    StartSession(impl_);
    std::fprintf(stderr,
        "unio-pipe: WGC started %dx%d (primary monitor, CPU path)\n",
        impl_->width, impl_->height);
    return true;
}

bool WgcCapture::StartGpu(WgcRect rect, int /*fps*/,
                           GpuFrameCallback cb, void* user) {
    impl_->gpu_cb = cb;
    impl_->user = user;
    if (!SetupMonitor(*impl_, rect)) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(impl_->width);
    td.Height = static_cast<UINT>(impl_->height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE
                 | D3D11_BIND_RENDER_TARGET;
    td.CPUAccessFlags = 0;
    td.MiscFlags = 0;
    for (int i = 0; i < Impl::kGpuPoolCount; ++i) {
        if (FAILED(impl_->device->CreateTexture2D(
                &td, nullptr, &impl_->gpu_pool[i]))) {
            return false;
        }
    }

    StartSession(impl_);
    std::fprintf(stderr,
        "unio-pipe: WGC started %dx%d (primary monitor, GPU zero-copy)\n",
        impl_->width, impl_->height);
    return true;
}

void WgcCapture::Close() {
    // Hold frame_mu across the entire teardown. Any in-flight
    // OnFrame is already holding it; we block until it returns.
    // Any OnFrame that arrives AFTER our lock release sees
    // running=false and returns without touching the (now-reset)
    // D3D11 resources. Covers both race windows:
    //   1. WGC's FrameArrived unregistration isn't guaranteed to
    //      flush in-flight invocations — worker thread may still
    //      dispatch the handler for a short window after we call
    //      pool.FrameArrived(token).
    //   2. pool.Close() itself may synchronously flush queued
    //      callbacks, and those callbacks are allowed to reach
    //      our handler on the same thread.
    std::lock_guard<std::mutex> lk(impl_->frame_mu);
    if (impl_->running.exchange(false,
            std::memory_order_acq_rel)) {
        try {
            if (impl_->pool) impl_->pool.FrameArrived(impl_->frame_token);
            if (impl_->session) impl_->session.Close();
            if (impl_->pool) impl_->pool.Close();
        } catch (...) {}
    }
    impl_->session = nullptr;
    impl_->pool = nullptr;
    impl_->item = nullptr;
    impl_->winrt_device = nullptr;
    impl_->staging.Reset();
    impl_->ctx.Reset();
    impl_->device.Reset();
    if (impl_->com_inited) {
        ::RoUninitialize();
        impl_->com_inited = false;
    }
}

}  // namespace unio

#endif  // _WIN32
