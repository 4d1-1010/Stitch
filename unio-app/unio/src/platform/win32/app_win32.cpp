/// @file app_win32.cpp
/// @brief Win32 window + D3D11 swap chain + ImGui Win32/DX11 backends.

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "orchestrator/orchestrator.hpp"
#include "platform/app.hpp"
#include "theme/logo.hpp"
#include "theme/palette.hpp"
#include "theme/style.hpp"
#include "ui/shell.hpp"
#include "util/image_loader.hpp"

#include "logo_mark_48.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

/// @brief Upstream ImGui helper; translates Win32 messages into IO events.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace unio_ui::platform {

namespace {

constexpr wchar_t kWndClass[] = L"UnIOWindowClass";

/// @brief Bundle of Win32 + D3D11 state owned by the run loop.
struct Win32App {
    HINSTANCE                      hinst = nullptr;
    HWND                           hwnd  = nullptr;
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    ctx;
    ComPtr<IDXGISwapChain1>        swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    int  width        = 0;
    int  height       = 0;
    bool should_close = false;
    bool needs_resize = false;
};

/// @brief Singleton pointer used by @ref wndproc to reach the
/// currently-running app state.
Win32App* g_app = nullptr;

/// @brief Convert a UTF-8 string to a wide string for Win32 APIs.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

/// @brief Top-level window procedure; routes messages through ImGui
/// first, then handles close / resize / destroy.
LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l)) return 0;
    switch (msg) {
        case WM_CLOSE:
            if (g_app) g_app->should_close = true;
            return 0;
        case WM_SIZE:
            if (g_app && w != SIZE_MINIMIZED) {
                g_app->width        = LOWORD(l);
                g_app->height       = HIWORD(l);
                g_app->needs_resize = true;
            }
            return 0;
        // Suppress the OS-level erase. The class brush already
        // matches our paper background so the modal-resize loop's
        // pre-paint phase looks identical to the app bg, and
        // returning TRUE here tells Windows it doesn't need to
        // erase a second time before our next D3D11 present.
        // Without this, a fast resize lets the OS-clear flash
        // visible between drag updates.
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, w, l);
    }
}

/// @brief Register the window class and create the top-level HWND.
bool init_window(Win32App& app, const AppConfig& cfg) {
    app.hinst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = app.hinst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    // Background brush set to the paper-bg colour so the modal-
    // resize-loop's pre-paint clear matches what the app renders;
    // combined with the WM_ERASEBKGND short-circuit in `wndproc`
    // this eliminates the black-flash on fast resize.
    wc.hbrBackground = CreateSolidBrush(RGB(0xff, 0xff, 0xff));
    if (!RegisterClassExW(&wc)) return false;

    RECT rc{0, 0, cfg.window_width, cfg.window_height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const std::wstring title = widen(cfg.window_title);
    app.hwnd = CreateWindowExW(
        0, kWndClass, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, app.hinst, nullptr);
    if (!app.hwnd) return false;

    app.width  = cfg.window_width;
    app.height = cfg.window_height;
    ShowWindow(app.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(app.hwnd);
    return true;
}

/// @brief Create the D3D11 device + DXGI swap chain bound to the HWND.
bool init_d3d11(Win32App& app) {
    const D3D_FEATURE_LEVEL fls[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        fls, ARRAYSIZE(fls), D3D11_SDK_VERSION,
        app.device.GetAddressOf(), nullptr, app.ctx.GetAddressOf());
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgi_dev;
    app.device.As(&dxgi_dev);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(adapter.GetAddressOf());
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = app.width;
    sd.Height           = app.height;
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = 2;
    sd.Scaling          = DXGI_SCALING_STRETCH;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(
        app.device.Get(), app.hwnd, &sd, nullptr, nullptr,
        app.swap.GetAddressOf());
    return SUCCEEDED(hr);
}

/// @brief Bind the swap chain's back buffer as a render target view.
bool create_rtv(Win32App& app) {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(app.swap->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf())))) {
        return false;
    }
    return SUCCEEDED(app.device->CreateRenderTargetView(
        back.Get(), nullptr, app.rtv.GetAddressOf()));
}

/// @brief Resize the swap-chain back buffer after a WM_SIZE.
void handle_resize(Win32App& app) {
    if (!app.needs_resize) return;
    app.rtv.Reset();
    app.swap->ResizeBuffers(0, app.width, app.height,
                            DXGI_FORMAT_UNKNOWN, 0);
    create_rtv(app);
    app.needs_resize = false;
}

/// @brief Run one full ImGui frame + clear + present.
void render_frame(Win32App& app, ui::Shell& shell) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    shell.render();

    ImGui::Render();

    const ImVec4 bg = theme::palette::paper_bg;
    const float clear[4] = {bg.x, bg.y, bg.z, bg.w};
    app.ctx->OMSetRenderTargets(1, app.rtv.GetAddressOf(), nullptr);
    app.ctx->ClearRenderTargetView(app.rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    app.swap->Present(1, 0);
}

/// @brief Upload an RGBA8 buffer to a D3D11 shader-resource-view.
ID3D11ShaderResourceView* upload_rgba_srv(ID3D11Device* dev,
                                          const unsigned char* pixels,
                                          int w, int h) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem     = pixels;
    sd.SysMemPitch = w * 4;

    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(dev->CreateTexture2D(&td, &sd, tex.GetAddressOf()))) {
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format              = td.Format;
    svd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(dev->CreateShaderResourceView(tex.Get(), &svd, &srv))) {
        return nullptr;
    }
    return srv;
}

/// @brief Decode and upload the embedded logo; register with the theme.
void load_logo_texture(ID3D11Device* dev) {
    auto img = unio_ui::decode_image(logo_mark_48_data, logo_mark_48_size);
    if (!img.pixels) return;
    ID3D11ShaderResourceView* srv = upload_rgba_srv(
        dev, img.pixels, img.width, img.height);
    unio_ui::free_decoded_image(img);
    if (!srv) return;
    // ImTextureID is an integer alias in ImGui ≥ 1.92, so the
    // pointer value goes through uintptr_t → ImTextureID via
    // static_cast (MSVC rejects reinterpret_cast between an
    // integer type and an integer alias).
    theme::register_logo_texture(
        static_cast<ImTextureID>(
            reinterpret_cast<std::uintptr_t>(srv)),
        48, 48);
}

}  // namespace

int run(const AppConfig& cfg) {
    Win32App app;
    g_app = &app;

    if (!init_window(app, cfg) || !init_d3d11(app) || !create_rtv(app)) {
        g_app = nullptr;
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    theme::load_fonts();
    theme::apply_style();
    if (!ImGui_ImplWin32_Init(app.hwnd) ||
        !ImGui_ImplDX11_Init(app.device.Get(), app.ctx.Get())) {
        g_app = nullptr;
        return 1;
    }

    load_logo_texture(app.device.Get());
    auto orch = orchestrator::make_mock({});
    ui::Shell shell(*orch);
    MSG msg{};
    while (!app.should_close) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                app.should_close = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (app.should_close) break;
        handle_resize(app);
        render_frame(app, shell);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_app = nullptr;
    return 0;
}

}  // namespace unio_ui::platform
