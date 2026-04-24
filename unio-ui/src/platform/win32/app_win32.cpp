/*! @file app_win32.cpp
 *  @brief Win32 + D3D11 platform backend.
 *
 *  CreateWindowExW + D3D11 device + swap chain + message pump,
 *  driving ImGui via upstream `imgui_impl_win32` (input) +
 *  `imgui_impl_dx11` (render). Renders the ImGui demo window
 *  every frame as a smoke test.
 */

#include "../app.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "../../screens/shell.hpp"
#include "../../theme.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

// Forward decl from imgui_impl_win32.cpp — upstream helper that
// translates Win32 messages into ImGui IO events.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace unio_ui::platform {

namespace {

constexpr wchar_t kWndClass[] = L"UnIOWindowClass";

struct Win32App {
    HINSTANCE hinst = nullptr;
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    int width = 0;
    int height = 0;
    bool should_close = false;
    bool needs_resize = false;
};

Win32App* g_app = nullptr;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    // ImGui's Win32 backend wants first crack at every message so
    // it can track mouse + keyboard + focus + IME state.
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w, l)) {
        return 0;
    }
    switch (msg) {
        case WM_CLOSE:
            if (g_app) g_app->should_close = true;
            return 0;
        case WM_SIZE:
            if (g_app && w != SIZE_MINIMIZED) {
                g_app->width = LOWORD(l);
                g_app->height = HIWORD(l);
                g_app->needs_resize = true;
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, w, l);
    }
}

bool init_window(Win32App& app, const AppConfig& cfg) {
    app.hinst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = app.hinst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc)) {
        std::fprintf(stderr, "RegisterClassExW failed\n");
        return false;
    }

    RECT rc{0, 0, cfg.window_width, cfg.window_height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const std::wstring title = widen(cfg.window_title);
    app.hwnd = CreateWindowExW(
        0, kWndClass, title.c_str(), WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, app.hinst, nullptr);
    if (!app.hwnd) {
        std::fprintf(stderr, "CreateWindowExW failed\n");
        return false;
    }
    app.width = cfg.window_width;
    app.height = cfg.window_height;
    ShowWindow(app.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(app.hwnd);
    return true;
}

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
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11CreateDevice failed hr=0x%08lx\n", hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_dev;
    app.device.As(&dxgi_dev);
    ComPtr<IDXGIAdapter> adapter;
    dxgi_dev->GetAdapter(adapter.GetAddressOf());
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = app.width;
    sd.Height = app.height;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(
        app.device.Get(), app.hwnd, &sd, nullptr, nullptr,
        app.swap.GetAddressOf());
    if (FAILED(hr)) {
        std::fprintf(stderr, "CreateSwapChainForHwnd failed hr=0x%08lx\n", hr);
        return false;
    }
    return true;
}

bool create_rtv(Win32App& app) {
    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = app.swap->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()));
    if (FAILED(hr)) return false;
    hr = app.device->CreateRenderTargetView(
        back.Get(), nullptr, app.rtv.GetAddressOf());
    return SUCCEEDED(hr);
}

void handle_resize(Win32App& app) {
    if (!app.needs_resize) return;
    app.rtv.Reset();
    app.swap->ResizeBuffers(0, app.width, app.height,
                            DXGI_FORMAT_UNKNOWN, 0);
    create_rtv(app);
    app.needs_resize = false;
}

void render_frame(Win32App& app, unio_ui::screens::Shell& shell) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    shell.render();

    ImGui::Render();

    const float clear[4] = {
        0xf6 / 255.f, 0xf3 / 255.f, 0xec / 255.f, 1.f};  // paper-bg
    app.ctx->OMSetRenderTargets(1, app.rtv.GetAddressOf(), nullptr);
    app.ctx->ClearRenderTargetView(app.rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    app.swap->Present(1, 0);
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
    unio_ui::theme::apply_style();
    if (!ImGui_ImplWin32_Init(app.hwnd) ||
        !ImGui_ImplDX11_Init(app.device.Get(), app.ctx.Get())) {
        std::fprintf(stderr, "ImGui backend init failed\n");
        g_app = nullptr;
        return 1;
    }

    unio_ui::screens::Shell shell;
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
