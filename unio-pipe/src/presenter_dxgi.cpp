// DXGI flip-model presenter — Windows sibling of
// presenter_egl_x11.cpp. A borderless HWND + an
// IDXGISwapChain1 (FLIP_DISCARD, SyncInterval=0) tear-presents
// the NV12 texture delivered by the D3D11VA decoder.
//
// Scope of Day 8e today:
//   * HWND + swap chain + D3D11 device + render thread
//   * Accepts DecodedFrame references (textures owned by
//     the decoder) and issues Present()
//   * Per-frame clear-colour placeholder — the real NV12
//     sampling shader lands in Day 8e-b, matching how Day 7a
//     landed the EGL presenter before Day 7b added DMA-BUF.
//
// The DecodedFrame's `native_device` is the decoder's
// ID3D11Device* — we accept it and use it so decoder + swap
// chain share the same device without a keyed-mutex dance.

#if !defined(_WIN32)
#include "presenter.h"
namespace unio {
std::unique_ptr<Presenter> MakeDxgiFlipPresenter() { return nullptr; }
}  // namespace unio
#else

#include "presenter.h"

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
#include <dxgi1_2.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace unio {

namespace {

const wchar_t* kWindowClass = L"unio-pipe-sink-window";

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
        // 1. Register window class + create borderless popup HWND.
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWindowClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);  // Ignore re-register ERROR_CLASS_ALREADY_EXISTS

        const int w = cfg_.width > 0 ? cfg_.width : 1920;
        const int h = cfg_.height > 0 ? cfg_.height : 1080;
        std::wstring title(cfg_.window_title.begin(),
                           cfg_.window_title.end());
        hwnd_ = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            kWindowClass, title.c_str(),
            WS_POPUP | WS_VISIBLE,
            cfg_.x, cfg_.y, w, h,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!hwnd_) return "CreateWindowEx failed";

        // 2. D3D11 device. Borrow the decoder's device when the
        // first DecodedFrame arrives rather than creating our own,
        // so decoded textures + the swap chain sit on the same
        // device and we can sample without a keyed mutex. For
        // the pre-first-frame init and the Day 8e clear-colour
        // path, we spin up our own device. Swapped out when the
        // first real frame lands (Day 8e-b).
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL got = {};
        D3D_FEATURE_LEVEL wanted[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            wanted, 2, D3D11_SDK_VERSION,
            &device_, &got, &ctx_);
        if (FAILED(hr)) return "D3D11CreateDevice failed";

        // 3. Swap chain via DXGIFactory2::CreateSwapChainForHwnd.
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
        scd.Width = static_cast<UINT>(w);
        scd.Height = static_cast<UINT>(h);
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = 2;
        scd.Scaling = DXGI_SCALING_STRETCH;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        scd.Flags = 0;
        if (FAILED(hr = factory->CreateSwapChainForHwnd(
                device_.Get(), hwnd_, &scd, nullptr, nullptr,
                &swap_chain_))) {
            return "CreateSwapChainForHwnd failed";
        }
        // Never block on the Alt-Enter monitor-info update.
        factory->MakeWindowAssociation(hwnd_,
            DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

        // 4. First paint so ProbeRoot-style observers see content
        // (matches the EGL presenter's paint-on-init fix).
        PaintClearColour(0.05f, 0.05f, 0.10f);

        {
            std::lock_guard<std::mutex> lk(ready_mu_);
            init_ready_.store(true, std::memory_order_release);
            ready_cv_.notify_all();
        }
        std::fprintf(stderr,
            "unio-pipe: DXGI flip presenter %dx%d, "
            "feature_level=0x%x, HWND=%p\n",
            w, h, static_cast<unsigned>(got), hwnd_);

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
            RenderFrame(frame);
        }
        return std::nullopt;
    }

    void PaintClearColour(float r, float g, float b) {
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swap_chain_->GetBuffer(
                0, IID_PPV_ARGS(&back)))) return;
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device_->CreateRenderTargetView(
                back.Get(), nullptr, &rtv))) return;
        const float c[4] = {r, g, b, 1.0f};
        ctx_->ClearRenderTargetView(rtv.Get(), c);
        swap_chain_->Present(0, 0);
    }

    void RenderFrame(const DecodedFrame& frame) {
        // Day 8e scope: clear to a frame-id-derived colour so a
        // viewer sees motion proving frames are arriving. Day 8e-b
        // replaces this with a VideoProcessor + ShaderResourceView
        // path that samples the decoder's NV12 texture at
        // `frame.surface_handle`.
        const auto n = frames_presented_.load() % 120;
        const float t = static_cast<float>(n) / 120.0f;
        PaintClearColour(t, 1.0f - t, 0.5f + 0.5f * t);
        frames_presented_.fetch_add(1, std::memory_order_relaxed);
        (void)frame;
    }

    void Shutdown() {
        run_.store(false, std::memory_order_release);
        queue_cv_.notify_all();
        if (present_thread_.joinable()) present_thread_.join();
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
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_chain_;

    std::atomic<std::uint64_t> frames_presented_{0};
};

}  // namespace

std::unique_ptr<Presenter> MakeDxgiFlipPresenter() {
    return std::make_unique<DxgiFlipPresenter>();
}

}  // namespace unio

#endif  // _WIN32
