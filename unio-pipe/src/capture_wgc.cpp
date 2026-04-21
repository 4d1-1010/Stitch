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
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count());
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
    void* user = nullptr;
    int width = 0;
    int height = 0;
    bool com_inited = false;

    // Called from a msquic / WGC worker thread on every arrival.
    // Must not block the thread for long — CopyResource + Map +
    // memcpy is ~3-5 ms at 1080p, well under the 16 ms frame
    // budget.
    void OnFrame() {
        if (!running.load(std::memory_order_acquire)) return;
        auto frame = pool.TryGetNextFrame();
        if (!frame) return;
        auto surface = frame.Surface();
        auto tex = TextureFromSurface(surface);
        if (!tex) return;

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
        cf->capture_monotonic_ns = NowNs();
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

WgcCapture::WgcCapture() : impl_(std::make_unique<Impl>()) {}
WgcCapture::~WgcCapture() { Close(); }

bool WgcCapture::Open() {
    HRESULT hr = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        impl_->com_inited = (hr == S_OK);
    } else {
        return false;
    }

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
    return true;
}

bool WgcCapture::Start(WgcRect rect, int /*fps*/,
                        FrameCallback cb, void* user) {
    impl_->cb = cb;
    impl_->user = user;
    impl_->width = rect.width;
    impl_->height = rect.height;

    HMONITOR hmon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    auto factory = winrt::get_activation_factory<
        wgc::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();
    HRESULT hr = factory->CreateForMonitor(
        hmon, winrt::guid_of<wgc::GraphicsCaptureItem>(),
        winrt::put_abi(impl_->item));
    if (FAILED(hr) || !impl_->item) {
        std::fprintf(stderr,
            "unio-pipe: WGC CreateForMonitor failed HR=0x%x\n",
            static_cast<unsigned>(hr));
        return false;
    }
    auto item_size = impl_->item.Size();
    if (impl_->width  == 0) impl_->width  = item_size.Width;
    if (impl_->height == 0) impl_->height = item_size.Height;

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

    impl_->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        impl_->winrt_device,
        wgd::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2, {impl_->width, impl_->height});

    impl_->running.store(true, std::memory_order_release);

    auto* raw = impl_.get();
    impl_->frame_token = impl_->pool.FrameArrived(
        [raw](wgc::Direct3D11CaptureFramePool const&,
              winrt::Windows::Foundation::IInspectable const&) {
            raw->OnFrame();
        });

    impl_->session = impl_->pool.CreateCaptureSession(impl_->item);
    try {
        impl_->session.IsCursorCaptureEnabled(true);
    } catch (...) {}
    impl_->session.StartCapture();

    std::fprintf(stderr,
        "unio-pipe: WGC started %dx%d (primary monitor)\n",
        impl_->width, impl_->height);
    return true;
}

void WgcCapture::Close() {
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
