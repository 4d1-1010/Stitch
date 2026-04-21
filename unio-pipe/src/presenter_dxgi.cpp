// DXGI flip-model presenter — Windows sibling of
// presenter_egl_x11.cpp. A borderless HWND + an
// IDXGISwapChain1 (FLIP_DISCARD, SyncInterval=0) tear-presents
// the NV12 texture delivered by the D3D11VA decoder.
//
// Rendering path:
//   decoder ID3D11Device  ─►  IDXGISwapChain1 created on the
//           │                 SAME device, so the decoded
//           │                 ID3D11Texture2D (NV12) is
//           │                 directly accessible as a shader
//           │                 resource without cross-device
//           │                 sharing.
//           ▼
//   ID3D11ShaderResourceView on Y plane (DXGI_FORMAT_R8_UNORM)
//   ID3D11ShaderResourceView on UV plane (DXGI_FORMAT_R8G8_UNORM)
//           ▼
//   Fullscreen triangle (vertex shader generates from
//   SV_VertexID — no VBO needed)
//           ▼
//   Pixel shader samples both, applies BT.601 limited-range
//   YUV→RGB conversion, writes to the swap chain backbuffer.
//           ▼
//   IDXGISwapChain1::Present(0, 0) — SyncInterval=0, tear-
//   present.
//
// We defer D3D11Device creation until the first DecodedFrame
// arrives and adopt its `native_device`. Everything else (HWND,
// window class) is stood up eagerly so the sink is visible
// before any frames land.

#if !defined(_WIN32)
#include "presenter.h"
namespace unio {
std::unique_ptr<Presenter> MakeDxgiFlipPresenter() { return nullptr; }
}  // namespace unio
#else

#include "presenter.h"
#include "latency_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace unio {

namespace {

const wchar_t* kWindowClass = L"unio-pipe-sink-window";

// Shared HLSL: vertex shader generates a fullscreen triangle
// from SV_VertexID (no VBO / IB / input layout needed — three
// vertex IDs cover the screen via the standard oversized-triangle
// trick). Pixel shader samples Y from slot 0 and UV from slot 1
// and does BT.601 limited-range YUV→RGB.
const char* kVsSrc = R"(
void VSMain(uint vid : SV_VertexID,
            out float4 pos : SV_Position,
            out float2 tc  : TEXCOORD0)
{
    float2 p;
    p.x = (vid == 2) ? 3.0 : -1.0;
    p.y = (vid == 1) ? -3.0 : 1.0;
    pos = float4(p, 0.0, 1.0);
    tc  = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
}
)";

const char* kPsSrc = R"(
Texture2D<float>  y_tex  : register(t0);
Texture2D<float2> uv_tex : register(t1);
SamplerState      smp    : register(s0);

float4 PSMain(float4 pos : SV_Position,
              float2 tc  : TEXCOORD0) : SV_Target
{
    float  y  = y_tex.Sample(smp, tc);
    float2 uv = uv_tex.Sample(smp, tc) - 0.5;
    y = (y - 16.0/255.0) * (255.0/219.0);
    float3 rgb;
    rgb.r = y + 1.402    * uv.y;
    rgb.g = y - 0.344136 * uv.x - 0.714136 * uv.y;
    rgb.b = y + 1.772    * uv.x;
    return float4(rgb, 1.0);
}
)";

class DxgiFlipPresenter final : public Presenter {
public:
    DxgiFlipPresenter() = default;
    ~DxgiFlipPresenter() override { Shutdown(); }

    std::optional<std::string> Init(const Config& cfg) override {
        cfg_ = cfg;
        run_.store(true, std::memory_order_release);
        present_thread_ = std::thread([this]() {
            if (auto err = RunPresentLoop(); err) {
                std::lock_guard<std::mutex> lk(err_mu_);
                init_error_ = *err;
            }
            {
                std::lock_guard<std::mutex> lk(ready_mu_);
                init_done_.store(true, std::memory_order_release);
                ready_cv_.notify_all();
            }
        });
        std::unique_lock<std::mutex> lk(ready_mu_);
        ready_cv_.wait(lk, [&]() {
            return init_ready_.load() || init_done_.load();
        });
        std::lock_guard<std::mutex> elk(err_mu_);
        if (!init_error_.empty()) return init_error_;
        return std::nullopt;
    }

    void Present(const DecodedFrame& frame) override {
        {
            std::lock_guard<std::mutex> lk(queue_mu_);
            if (queue_.size() >= 2) queue_.pop_front();
            queue_.push_back(frame);
        }
        queue_cv_.notify_one();
    }

    std::uint64_t FramesPresented() const override {
        return frames_presented_.load(std::memory_order_relaxed);
    }

    std::string_view Name() const override { return "dxgi-flip"; }

private:
    std::optional<std::string> RunPresentLoop() {
        // 1. Register window class + create borderless popup HWND
        //    before we have a device — the window is visible
        //    immediately so a watcher sees it at start_inbound
        //    time, not only after decode begins.
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);

        window_w_ = cfg_.width > 0 ? cfg_.width : 1920;
        window_h_ = cfg_.height > 0 ? cfg_.height : 1080;
        std::wstring title(cfg_.window_title.begin(),
                           cfg_.window_title.end());
        hwnd_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            kWindowClass, title.c_str(),
            WS_POPUP | WS_VISIBLE,
            cfg_.x, cfg_.y, window_w_, window_h_,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd_) return "CreateWindowEx failed";

        // 2. Device / swap chain / pipeline are built lazily
        //    when the first DecodedFrame arrives. See
        //    BuildPipeline().
        {
            std::lock_guard<std::mutex> lk(ready_mu_);
            init_ready_.store(true, std::memory_order_release);
            ready_cv_.notify_all();
        }
        std::fprintf(stderr,
            "unio-pipe: DXGI flip presenter %dx%d, HWND=%p "
            "(pipeline pending first frame)\n",
            window_w_, window_h_, hwnd_);

        while (run_.load(std::memory_order_acquire)) {
            DecodedFrame frame{};
            bool have = false;
            {
                std::unique_lock<std::mutex> lk(queue_mu_);
                queue_cv_.wait_for(lk,
                    std::chrono::milliseconds(16), [&]() {
                        return !queue_.empty() || !run_.load();
                    });
                if (!queue_.empty()) {
                    frame = queue_.front();
                    queue_.pop_front();
                    have = true;
                }
            }
            if (!have) continue;
            if (!pipeline_ready_) {
                if (auto err = BuildPipeline(frame); err) {
                    std::fprintf(stderr,
                        "unio-pipe: BuildPipeline failed: %s\n",
                        err->c_str());
                    // Keep loop alive — counter still reports
                    // we're draining frames; user sees a black
                    // window instead of a crash.
                    frames_presented_.fetch_add(
                        1, std::memory_order_relaxed);
                    continue;
                }
            }
            RenderFrame(frame);
        }
        return std::nullopt;
    }

    std::optional<std::string> BuildPipeline(const DecodedFrame& frame) {
        // 1. Adopt the decoder's ID3D11Device — the DecodedFrame
        //    carries it explicitly precisely so the presenter
        //    can sample the decoder's NV12 texture without a
        //    cross-device keyed-mutex share.
        auto* dev = reinterpret_cast<ID3D11Device*>(
            frame.native_device);
        if (!dev) return "DecodedFrame.native_device is null";
        device_ = dev;
        device_->GetImmediateContext(&ctx_);

        // 2. Swap chain on that same device.
        ComPtr<IDXGIDevice> dxgi_dev;
        if (FAILED(device_.As(&dxgi_dev))) {
            return "ID3D11Device→IDXGIDevice QI failed";
        }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgi_dev->GetAdapter(&adapter))) {
            return "IDXGIDevice::GetAdapter failed";
        }
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return "adapter→IDXGIFactory2 failed";
        }
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = static_cast<UINT>(window_w_);
        scd.Height = static_cast<UINT>(window_h_);
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scd.Flags = 0;
        HRESULT hr = factory->CreateSwapChainForHwnd(
            device_.Get(), hwnd_, &scd, nullptr, nullptr,
            &swap_chain_);
        if (FAILED(hr)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                "CreateSwapChainForHwnd HR=0x%08x",
                static_cast<unsigned>(hr));
            return std::string(buf);
        }
        factory->MakeWindowAssociation(hwnd_,
            DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

        // 3. Compile shaders at runtime. D3DCompile is heavier
        //    than precompiling to DXBC, but keeps the build
        //    simple (no fxc.exe pipeline) and still runs once
        //    at first-frame.
        ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
        if (FAILED(D3DCompile(kVsSrc, std::strlen(kVsSrc), nullptr,
                              nullptr, nullptr, "VSMain", "vs_5_0",
                              0, 0, &vs_blob, &err_blob))) {
            return std::string("VS compile: ")
                   + (err_blob ? static_cast<const char*>(
                         err_blob->GetBufferPointer()) : "?");
        }
        if (FAILED(D3DCompile(kPsSrc, std::strlen(kPsSrc), nullptr,
                              nullptr, nullptr, "PSMain", "ps_5_0",
                              0, 0, &ps_blob, &err_blob))) {
            return std::string("PS compile: ")
                   + (err_blob ? static_cast<const char*>(
                         err_blob->GetBufferPointer()) : "?");
        }
        if (FAILED(device_->CreateVertexShader(
                vs_blob->GetBufferPointer(),
                vs_blob->GetBufferSize(), nullptr, &vs_))) {
            return "CreateVertexShader failed";
        }
        if (FAILED(device_->CreatePixelShader(
                ps_blob->GetBufferPointer(),
                ps_blob->GetBufferSize(), nullptr, &ps_))) {
            return "CreatePixelShader failed";
        }

        // 4. Sampler — linear filtering, clamp edges so the
        //    upper-right triangle corner doesn't wrap.
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) {
            return "CreateSamplerState failed";
        }

        // Rasterizer: cull NONE. Our SV_VertexID fullscreen triangle
        // ends up counter-clockwise in D3D11 screen space, which
        // the default rasterizer (FrontCounterClockwise=FALSE,
        // CullMode=BACK) culls entirely — the first run rendered
        // only the clear color because every pixel got dropped.
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) {
            return "CreateRasterizerState failed";
        }

        // 5. Viewport matches the swap chain backbuffer.
        viewport_.Width    = static_cast<FLOAT>(window_w_);
        viewport_.Height   = static_cast<FLOAT>(window_h_);
        viewport_.MinDepth = 0.0f;
        viewport_.MaxDepth = 1.0f;

        pipeline_ready_ = true;
        std::fprintf(stderr,
            "unio-pipe: DXGI pipeline ready (shared D3D11 device)\n");
        return std::nullopt;
    }

    void RenderFrame(const DecodedFrame& frame) {
        auto* nv12 = reinterpret_cast<ID3D11Texture2D*>(
            frame.surface_handle);
        if (!nv12) {
            frames_presented_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Create Y / UV SRVs on the decoded NV12 texture. D3D11
        // lets us create typed views into the individual planes of
        // an NV12 texture by specifying R8_UNORM (Y plane) and
        // R8G8_UNORM (UV plane). New SRVs every frame because the
        // decoder rotates its NV12 pool and we'd otherwise stale-
        // reference a surface that's since been recycled. Cost is
        // ~10 µs per CreateSRV on Intel UHD 630.
        D3D11_SHADER_RESOURCE_VIEW_DESC ysrv{};
        ysrv.Format = DXGI_FORMAT_R8_UNORM;
        ysrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        ysrv.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> y_srv;
        if (FAILED(device_->CreateShaderResourceView(
                nv12, &ysrv, &y_srv))) return;

        D3D11_SHADER_RESOURCE_VIEW_DESC uvsrv{};
        uvsrv.Format = DXGI_FORMAT_R8G8_UNORM;
        uvsrv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        uvsrv.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> uv_srv;
        if (FAILED(device_->CreateShaderResourceView(
                nv12, &uvsrv, &uv_srv))) return;

        // Render target = swap chain backbuffer.
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swap_chain_->GetBuffer(
                0, IID_PPV_ARGS(&back)))) return;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device_->CreateRenderTargetView(
                back.Get(), nullptr, &rtv))) return;

        ID3D11RenderTargetView* rtvs[] = {rtv.Get()};
        ctx_->OMSetRenderTargets(1, rtvs, nullptr);
        ctx_->RSSetViewports(1, &viewport_);
        ctx_->RSSetState(raster_.Get());
        ctx_->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->IASetInputLayout(nullptr);   // no vertex buffer
        ID3D11Buffer* nullvb = nullptr;
        UINT stride = 0, offset = 0;
        ctx_->IASetVertexBuffers(0, 1, &nullvb, &stride, &offset);
        ctx_->VSSetShader(vs_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {y_srv.Get(), uv_srv.Get()};
        ctx_->PSSetShaderResources(0, 2, srvs);
        ID3D11SamplerState* samplers[] = {sampler_.Get()};
        ctx_->PSSetSamplers(0, 1, samplers);
        ctx_->Draw(3, 0);  // fullscreen triangle from SV_VertexID

        // Unbind SRVs so the decoder's next write to this
        // surface doesn't collide with a stale read binding.
        ID3D11ShaderResourceView* null_srvs[] = {nullptr, nullptr};
        ctx_->PSSetShaderResources(0, 2, null_srvs);

        swap_chain_->Present(0, 0);
        const std::uint64_t present_ns =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()).count());
        frames_presented_.fetch_add(1, std::memory_order_relaxed);
        if (frame.frame_id) {
            LogLatency("UNIO_PIPE_LATENCY_CSV",
                       frame.frame_id,
                       frame.capture_monotonic_ns,
                       frame.decode_done_monotonic_ns,
                       present_ns,
                       frame.width, frame.height);
        }
    }

    void Shutdown() {
        run_.store(false, std::memory_order_release);
        queue_cv_.notify_all();
        if (present_thread_.joinable()) present_thread_.join();
        raster_.Reset();
        sampler_.Reset();
        ps_.Reset();
        vs_.Reset();
        swap_chain_.Reset();
        ctx_.Reset();
        device_.Reset();
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    Config cfg_{};
    std::thread present_thread_;
    std::atomic<bool> run_{false};

    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<DecodedFrame> queue_;

    std::mutex ready_mu_;
    std::condition_variable ready_cv_;
    std::atomic<bool> init_ready_{false};
    std::atomic<bool> init_done_{false};
    std::mutex err_mu_;
    std::string init_error_;

    HWND hwnd_ = nullptr;
    int window_w_ = 0;
    int window_h_ = 0;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;
    D3D11_VIEWPORT viewport_{};
    bool pipeline_ready_ = false;

    std::atomic<std::uint64_t> frames_presented_{0};
};

}  // namespace

std::unique_ptr<Presenter> MakeDxgiFlipPresenter() {
    return std::make_unique<DxgiFlipPresenter>();
}

}  // namespace unio

#endif  // _WIN32
