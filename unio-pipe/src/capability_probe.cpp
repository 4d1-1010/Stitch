// Runtime capability probe — skeleton (#22 commit 1 of 7).
//
// This commit lands the struct + function surface. Every probe
// function returns available=false with reason="not_implemented"
// until the per-platform commits (#22 commits 2 + 3) fill them in.
// The streaming-block synthesiser lands in commit 4, JSON wiring
// in commit 5, stream_manager refusal in commit 6, env kill-switch
// in commit 7.
//
// Keeping per-backend probe functions public (declared in
// capability_probe.h) so WP 10's vendor sub-issues extend their
// own probe without touching this file's runner.

#include "capability_probe.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(__linux__)
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#if defined(UNIO_PIPE_HAS_VAAPI)
#include <va/va.h>
#include <va/va_drm.h>
#include <fcntl.h>
#endif

#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <EGL/egl.h>
#include <dirent.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

namespace unio {

const char* StreamingReasonName(StreamingReason r) {
    switch (r) {
        case StreamingReason::Available:    return "available";
        case StreamingReason::NoHwEncoder:  return "no_hw_encoder";
        case StreamingReason::NoCapture:    return "no_capture";
        case StreamingReason::ProbeError:   return "probe_error";
    }
    return "probe_error";
}

namespace {

BackendInfo NotImplemented(const std::string& name) {
    BackendInfo b;
    b.name = name;
    b.available = false;
    b.reason = "not_implemented";
    return b;
}

}  // namespace

// ── Per-platform probes — bodies land in commits 2 + 3 ─────────

#if defined(__linux__)

namespace {

// Path exists + readable? Used for /dev/dri/renderD128 +
// /dev/nvidia0 + the Wayland / D-Bus sockets. Avoids pulling
// std::filesystem just for a stat.
bool PathReadable(const char* p) {
    struct stat st{};
    return ::stat(p, &st) == 0;
}

// Read a single-line file and strip trailing whitespace. Returns
// empty string if read fails.
std::string ReadFirstLine(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line;
    std::getline(f, line);
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r' ||
            line.back() == ' '  || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

// Load a shared library via dlopen. Returns nullptr on failure
// without emitting to stderr — probes are quiet. The caller owns
// the handle and must dlclose when done (per-probe scope).
void* DlOpenQuiet(const char* soname) {
    return dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
}

const char* VendorFromPciId(const std::string& vendor_hex) {
    // PCI vendor IDs — the three we care about.
    if (vendor_hex == "0x10de") return "NVIDIA";
    if (vendor_hex == "0x1002") return "AMD";
    if (vendor_hex == "0x8086") return "Intel";
    return "Unknown";
}

}  // namespace

std::vector<AdapterInfo> EnumerateD3D11Adapters() { return {}; }

BackendInfo ProbeVaapi() {
    BackendInfo b;
    b.name = "vaapi";
    // /dev/dri/renderD128 is the kernel-side handle libva opens.
    // Without it nothing VA-API can do matters; exit the probe
    // quickly rather than pull the dispatcher for a negative.
    if (!PathReadable("/dev/dri/renderD128")) {
        b.reason = "no /dev/dri/renderD128 (missing kernel DRI or not readable)";
        return b;
    }
#if defined(UNIO_PIPE_HAS_VAAPI)
    int fd = ::open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) {
        b.reason = "open(/dev/dri/renderD128) failed";
        return b;
    }
    VADisplay dpy = vaGetDisplayDRM(fd);
    if (!dpy) {
        ::close(fd);
        b.reason = "vaGetDisplayDRM returned null";
        return b;
    }
    int maj = 0, min = 0;
    VAStatus st = vaInitialize(dpy, &maj, &min);
    if (st != VA_STATUS_SUCCESS) {
        vaTerminate(dpy);
        ::close(fd);
        b.reason = "vaInitialize failed";
        return b;
    }
    // Enumerate H.264 encode + decode profiles we care about.
    int nprofs = vaMaxNumProfiles(dpy);
    std::vector<VAProfile> profs(nprofs);
    int got = 0;
    vaQueryConfigProfiles(dpy, profs.data(), &got);
    bool has_enc = false, has_dec = false;
    for (int i = 0; i < got; ++i) {
        if (profs[i] == VAProfileH264ConstrainedBaseline
            || profs[i] == VAProfileH264Main
            || profs[i] == VAProfileH264High) {
            int neps = vaMaxNumEntrypoints(dpy);
            std::vector<VAEntrypoint> eps(neps);
            int egot = 0;
            vaQueryConfigEntrypoints(dpy, profs[i], eps.data(), &egot);
            for (int j = 0; j < egot; ++j) {
                if (eps[j] == VAEntrypointEncSlice
                    || eps[j] == VAEntrypointEncSliceLP) has_enc = true;
                if (eps[j] == VAEntrypointVLD) has_dec = true;
            }
        }
    }
    const char* vendor = vaQueryVendorString(dpy);
    b.notes = vendor ? vendor : "";
    vaTerminate(dpy);
    ::close(fd);
    if (!has_enc && !has_dec) {
        b.reason = "H.264 profiles absent in VA-API config";
        return b;
    }
    b.available = true;
    if (has_enc) b.codecs.emplace_back("h264");
#else
    // Built without libva at configure time. /dev/dri exists,
    // so the platform could support VA-API — but this helper
    // binary was built without it.
    b.reason = "helper built without libva (UNIO_PIPE_HAS_VAAPI=0)";
#endif
    return b;
}

BackendInfo ProbeXComposite() {
    BackendInfo b;
    b.name = "xcomposite";
    const char* display = std::getenv("DISPLAY");
    if (!display || !*display) {
        b.reason = "no DISPLAY set";
        return b;
    }
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        b.reason = "XOpenDisplay failed";
        return b;
    }
    int major = 0, event = 0, error = 0;
    const bool shm = XQueryExtension(dpy, "MIT-SHM",
                                      &major, &event, &error);
    int screen = DefaultScreen(dpy);
    const int w = DisplayWidth(dpy, screen);
    const int h = DisplayHeight(dpy, screen);
    XCloseDisplay(dpy);
    if (!shm) {
        b.reason = "MIT-SHM extension unavailable on this X server";
        return b;
    }
    b.available = true;
    std::ostringstream os;
    os << w << "x" << h;
    b.max_resolution = os.str();
    b.codecs.emplace_back("bgra");
    return b;
}

BackendInfo ProbeWaylandCapture() {
    BackendInfo b;
    b.name = "wayland-pipewire";
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (!wd || !*wd) {
        b.reason = "WAYLAND_DISPLAY not set";
        return b;
    }
    // Confirm the socket path is connectable — a stale env var
    // shouldn't light up the probe.
    std::string sock_path;
    if (wd[0] == '/') {
        sock_path = wd;
    } else if (xdg && *xdg) {
        sock_path = std::string(xdg) + "/" + wd;
    } else {
        b.reason = "XDG_RUNTIME_DIR unset, cannot resolve Wayland socket";
        return b;
    }
    if (!PathReadable(sock_path.c_str())) {
        b.reason = "Wayland socket path not found: " + sock_path;
        return b;
    }
    int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        b.reason = "socket() failed while probing Wayland display";
        return b;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof(addr.sun_path)) {
        ::close(s);
        b.reason = "Wayland socket path too long";
        return b;
    }
    std::strncpy(addr.sun_path, sock_path.c_str(),
                 sizeof(addr.sun_path) - 1);
    const bool connected =
        ::connect(s, reinterpret_cast<sockaddr*>(&addr),
                  sizeof(addr)) == 0;
    ::close(s);
    if (!connected) {
        b.reason = "Wayland socket not connectable";
        return b;
    }
    // Need an accessible session bus for xdg-desktop-portal.
    if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
        b.reason = "DBUS_SESSION_BUS_ADDRESS unset; portal unreachable";
        return b;
    }
    // Best-effort heuristic for xdg-desktop-portal presence. The
    // authoritative check is a D-Bus call to
    // org.freedesktop.portal.Desktop at start_outbound time (#7).
    // At probe time we just look for the package binary.
    if (PathReadable("/usr/libexec/xdg-desktop-portal")
        || PathReadable("/usr/lib/xdg-desktop-portal")
        || PathReadable("/usr/bin/xdg-desktop-portal")) {
        b.available = true;
        b.codecs.emplace_back("dmabuf");
        b.notes = "xdg-desktop-portal present; permission dialog fires on first start_outbound";
        return b;
    }
    b.reason = "xdg-desktop-portal binary not found on standard paths";
    return b;
}

BackendInfo ProbeWaylandPresenter() {
    BackendInfo b;
    b.name = "egl-wayland";
    const char* wd = std::getenv("WAYLAND_DISPLAY");
    if (!wd || !*wd) {
        b.reason = "WAYLAND_DISPLAY not set";
        return b;
    }
    // Need libwayland-client to actually render — the Wayland
    // presenter (#30) links it. At probe time we just check the
    // shared library is loadable without committing to link it.
    void* wl = DlOpenQuiet("libwayland-client.so.0");
    if (!wl) {
        b.reason = "libwayland-client.so.0 not loadable";
        return b;
    }
    dlclose(wl);
    b.available = true;
    b.codecs.emplace_back("nv12-dmabuf");
    b.notes = "libwayland-client.so.0 loadable; full init deferred to #30";
    return b;
}

BackendInfo ProbeEglX11Presenter() {
    BackendInfo b;
    b.name = "egl-x11";
    const char* display = std::getenv("DISPLAY");
    if (!display || !*display) {
        b.reason = "no DISPLAY set";
        return b;
    }
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        b.reason = "XOpenDisplay failed";
        return b;
    }
    EGLDisplay egl_dpy = eglGetDisplay(
        reinterpret_cast<EGLNativeDisplayType>(dpy));
    if (egl_dpy == EGL_NO_DISPLAY) {
        XCloseDisplay(dpy);
        b.reason = "eglGetDisplay(X11) returned EGL_NO_DISPLAY";
        return b;
    }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(egl_dpy, &maj, &min)) {
        XCloseDisplay(dpy);
        b.reason = "eglInitialize failed on X11 display";
        return b;
    }
    std::ostringstream notes;
    notes << "EGL " << maj << "." << min;
    b.notes = notes.str();
    eglTerminate(egl_dpy);
    XCloseDisplay(dpy);
    b.available = true;
    b.codecs.emplace_back("nv12-dmabuf");
    return b;
}

BackendInfo ProbeNvencLinuxRuntime() {
    BackendInfo b;
    b.name = "nvenc-linux";
    void* cuda = DlOpenQuiet("libcuda.so.1");
    if (!cuda) {
        b.reason = "libcuda.so.1 not loadable (no NVIDIA driver?)";
        return b;
    }
    void* enc = DlOpenQuiet("libnvidia-encode.so.1");
    if (!enc) {
        dlclose(cuda);
        b.reason = "libnvidia-encode.so.1 not loadable";
        return b;
    }
    // NvEncOpenEncodeSessionEx is the symbol every NVENC client
    // resolves. If it's absent the driver is too old for the API
    // version we target.
    void* sym = dlsym(enc, "NvEncodeAPICreateInstance");
    if (!sym) {
        dlclose(enc);
        dlclose(cuda);
        b.reason = "NvEncodeAPICreateInstance symbol missing";
        return b;
    }
    dlclose(enc);
    dlclose(cuda);
    b.available = true;
    b.codecs.emplace_back("h264");
    b.notes = "libcuda + libnvidia-encode loadable; may report false "
              "positive on misconfigured driver (e.g. nvidia-persistenced "
              "not running, nvidia-uvm module not loaded, kernel-module "
              "version mismatch). Actual session-open probe deferred to "
              "#27.";
    return b;
}

BackendInfo ProbeNvdecLinuxRuntime() {
    BackendInfo b;
    b.name = "nvdec-linux";
    void* cuda = DlOpenQuiet("libcuda.so.1");
    if (!cuda) {
        b.reason = "libcuda.so.1 not loadable (no NVIDIA driver?)";
        return b;
    }
    void* dec = DlOpenQuiet("libnvcuvid.so.1");
    if (!dec) {
        dlclose(cuda);
        b.reason = "libnvcuvid.so.1 not loadable";
        return b;
    }
    void* sym = dlsym(dec, "cuvidCreateVideoParser");
    if (!sym) {
        dlclose(dec);
        dlclose(cuda);
        b.reason = "cuvidCreateVideoParser symbol missing";
        return b;
    }
    dlclose(dec);
    dlclose(cuda);
    b.available = true;
    b.codecs.emplace_back("h264");
    b.notes = "libcuda + libnvcuvid loadable; may report false positive "
              "on misconfigured driver (same failure modes as nvenc-linux). "
              "Actual parser-init probe deferred to #21.";
    return b;
}

// Windows-only probes are stubs on Linux.
BackendInfo ProbeWgc()                { return NotImplemented("wgc"); }
BackendInfo ProbeNvencWindows()       { return NotImplemented("nvenc"); }
BackendInfo ProbeAmfRuntime()         { return NotImplemented("amf"); }
BackendInfo ProbeOneVplRuntime()      { return NotImplemented("onevpl"); }
BackendInfo ProbeD3d11va()            { return NotImplemented("d3d11va"); }
BackendInfo ProbeDxgiFlipPresenter()  { return NotImplemented("dxgi-flip"); }

#elif defined(_WIN32)

// Windows probe bodies. Shape mirrors the Linux side: open the
// minimum system resource, extract what can be reported, close
// cleanly. Expensive probes (NVENC session-open, AMF context
// creation) are deferred to their sub-issues; this file just
// verifies the runtime DLLs are loadable and the entry symbols
// resolve so the fallback chain in #24 has reliable capability
// bits to switch on.

namespace {

// Handy RAII for LoadLibrary. We prefer LoadLibraryW (UNICODE is
// defined in CMakeLists.txt).
class ModuleHandle {
public:
    explicit ModuleHandle(const wchar_t* name)
        : h_(::LoadLibraryW(name)) {}
    ~ModuleHandle() { if (h_) ::FreeLibrary(h_); }
    ModuleHandle(const ModuleHandle&) = delete;
    ModuleHandle& operator=(const ModuleHandle&) = delete;
    explicit operator bool() const { return h_ != nullptr; }
    HMODULE get() const { return h_; }
private:
    HMODULE h_;
};

std::string VendorFromDxgiId(UINT vendor_id) {
    switch (vendor_id) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x8086: return "Intel";
        case 0x1414: return "Microsoft";  // WARP / Basic Render Driver
        default:     return "Unknown";
    }
}

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0,
                                   nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n,
                           nullptr, nullptr);
    return out;
}

}  // namespace

// Linux-only probes stub out on Windows.
BackendInfo ProbeVaapi()              { return NotImplemented("vaapi"); }
BackendInfo ProbeXComposite()         { return NotImplemented("xcomposite"); }
BackendInfo ProbeWaylandCapture()     { return NotImplemented("wayland-pipewire"); }
BackendInfo ProbeWaylandPresenter()   { return NotImplemented("egl-wayland"); }
BackendInfo ProbeEglX11Presenter()    { return NotImplemented("egl-x11"); }
BackendInfo ProbeNvencLinuxRuntime()  { return NotImplemented("nvenc-linux"); }
BackendInfo ProbeNvdecLinuxRuntime()  { return NotImplemented("nvdec-linux"); }

std::vector<AdapterInfo> EnumerateD3D11Adapters() {
    std::vector<AdapterInfo> out;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(::CreateDXGIFactory1(
            __uuidof(IDXGIFactory1),
            reinterpret_cast<void**>(&factory)))) {
        return out;
    }
    UINT i = 0;
    IDXGIAdapter1* adapter = nullptr;
    while (factory->EnumAdapters1(i, &adapter) !=
           DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            AdapterInfo ai;
            ai.name = WideToUtf8(desc.Description);
            ai.vendor = VendorFromDxgiId(desc.VendorId);
            char luid[32];
            std::snprintf(luid, sizeof(luid), "%08lx%08lx",
                          static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                          static_cast<unsigned long>(desc.AdapterLuid.LowPart));
            ai.luid = luid;
            out.push_back(std::move(ai));
        }
        adapter->Release();
        ++i;
    }
    factory->Release();
    return out;
}

BackendInfo ProbeWgc() {
    BackendInfo b;
    b.name = "wgc";
    // WinRT IsSupported() is the canonical check. We reach it via
    // a RoGetActivationFactory call + QI for
    // IGraphicsCaptureSessionStatics; the full WinRT C++ pipe
    // used by capture_wgc.cpp is too heavy for a probe. Instead
    // probe indirectly: Windows 10 build >= 17134 has WGC; later
    // versions all do. Also confirm the WinRT runtime DLL loads.
    ModuleHandle combase(L"combase.dll");
    ModuleHandle d3d11rt(L"d3d11.dll");
    if (!combase || !d3d11rt) {
        b.reason = "combase.dll / d3d11.dll not loadable";
        return b;
    }
    OSVERSIONINFOEXW osv{};
    osv.dwOSVersionInfoSize = sizeof(osv);
    // RtlGetVersion bypasses the application-manifest guard that
    // makes GetVersionEx report Win8 on Win10+ for unmanifested
    // binaries. Available on every Windows 10/11 ntdll.
    ModuleHandle ntdll(L"ntdll.dll");
    if (!ntdll) {
        b.reason = "ntdll.dll unexpectedly unavailable";
        return b;
    }
    using RtlGetVersionPfn = LONG (WINAPI*)(OSVERSIONINFOEXW*);
    auto* fn = reinterpret_cast<RtlGetVersionPfn>(
        ::GetProcAddress(ntdll.get(), "RtlGetVersion"));
    if (!fn || fn(&osv) != 0) {
        b.reason = "RtlGetVersion failed";
        return b;
    }
    char notes[96];
    std::snprintf(notes, sizeof(notes),
                  "Windows %lu.%lu build %lu",
                  static_cast<unsigned long>(osv.dwMajorVersion),
                  static_cast<unsigned long>(osv.dwMinorVersion),
                  static_cast<unsigned long>(osv.dwBuildNumber));
    b.notes = notes;
    if (osv.dwMajorVersion < 10
        || (osv.dwMajorVersion == 10 && osv.dwBuildNumber < 17134)) {
        b.reason = std::string("WGC requires Windows 10 build 17134+; got ") + notes;
        return b;
    }
    b.available = true;
    b.codecs.emplace_back("bgra");
    return b;
}

BackendInfo ProbeNvencWindows() {
    BackendInfo b;
    b.name = "nvenc";
    ModuleHandle enc(L"nvEncodeAPI64.dll");
    if (!enc) {
        b.reason = "nvEncodeAPI64.dll not loadable (no NVIDIA driver?)";
        return b;
    }
    FARPROC sym = ::GetProcAddress(enc.get(),
                                    "NvEncodeAPICreateInstance");
    if (!sym) {
        b.reason = "NvEncodeAPICreateInstance symbol missing";
        return b;
    }
    b.available = true;
    b.codecs.emplace_back("h264");
    b.notes = "nvEncodeAPI64.dll loadable; session-open test deferred";
    return b;
}

BackendInfo ProbeAmfRuntime() {
    BackendInfo b;
    b.name = "amf";
    ModuleHandle amf(L"amfrt64.dll");
    if (!amf) {
        b.reason = "amfrt64.dll not loadable (no AMD driver?)";
        return b;
    }
    FARPROC sym = ::GetProcAddress(amf.get(), "AMFInit");
    if (!sym) {
        b.reason = "AMFInit symbol missing";
        return b;
    }
    b.available = true;
    b.codecs.emplace_back("h264");
    b.notes = "amfrt64.dll loadable; context-create deferred to #25";
    return b;
}

BackendInfo ProbeOneVplRuntime() {
    BackendInfo b;
    b.name = "onevpl";
    // oneVPL prefers libvpl.dll (modern); legacy libmfx.dll ships
    // with older Intel drivers. Either counts.
    ModuleHandle vpl(L"libvpl.dll");
    ModuleHandle mfx(L"libmfx.dll");
    if (!vpl && !mfx) {
        b.reason = "libvpl.dll / libmfx.dll not loadable";
        return b;
    }
    HMODULE hit = vpl ? vpl.get() : mfx.get();
    // Either library exposes MFXInit / MFXInitialize.
    FARPROC sym = ::GetProcAddress(hit, "MFXInit");
    if (!sym) sym = ::GetProcAddress(hit, "MFXInitialize");
    if (!sym) {
        b.reason = "MFXInit / MFXInitialize symbol missing";
        return b;
    }
    b.available = true;
    b.codecs.emplace_back("h264");
    b.notes = vpl
        ? "libvpl.dll (oneVPL) loadable; session-open deferred to #26"
        : "libmfx.dll (legacy MSDK) loadable; session-open deferred to #26";
    return b;
}

BackendInfo ProbeD3d11va() {
    BackendInfo b;
    b.name = "d3d11va";
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL wanted[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        wanted, static_cast<UINT>(std::size(wanted)),
        D3D11_SDK_VERSION, &device, &got, &ctx);
    if (FAILED(hr) || !device) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "D3D11CreateDevice(VIDEO) HR=0x%08x",
                      static_cast<unsigned>(hr));
        b.reason = buf;
        return b;
    }
    ID3D11VideoDevice* vdev = nullptr;
    hr = device->QueryInterface(
        __uuidof(ID3D11VideoDevice),
        reinterpret_cast<void**>(&vdev));
    if (FAILED(hr) || !vdev) {
        device->Release();
        if (ctx) ctx->Release();
        b.reason = "ID3D11VideoDevice QueryInterface failed";
        return b;
    }
    // GUID mirrors decoder_d3d11va.cpp::kH264VldNoFgt. Keeping
    // it literal here rather than cross-including to avoid a
    // compile-time dependency between probe and decoder.
    const GUID kH264VldNoFgt = {
        0x1b81be68, 0xa0c7, 0x11d3,
        {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};
    BOOL supported = FALSE;
    hr = vdev->CheckVideoDecoderFormat(
        &kH264VldNoFgt, DXGI_FORMAT_NV12, &supported);
    vdev->Release();
    device->Release();
    if (ctx) ctx->Release();
    if (FAILED(hr) || !supported) {
        b.reason = "H.264 VLD NOFGT + NV12 not supported by GPU";
        return b;
    }
    b.available = true;
    b.codecs.emplace_back("h264");
    return b;
}

BackendInfo ProbeDxgiFlipPresenter() {
    BackendInfo b;
    b.name = "dxgi-flip";
    IDXGIFactory2* f2 = nullptr;
    HRESULT hr = ::CreateDXGIFactory1(
        __uuidof(IDXGIFactory2),
        reinterpret_cast<void**>(&f2));
    if (FAILED(hr) || !f2) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "CreateDXGIFactory2 HR=0x%08x",
                      static_cast<unsigned>(hr));
        b.reason = buf;
        return b;
    }
    f2->Release();
    b.available = true;
    b.codecs.emplace_back("nv12");
    return b;
}

#else
BackendInfo ProbeVaapi()              { return NotImplemented("vaapi"); }
BackendInfo ProbeXComposite()         { return NotImplemented("xcomposite"); }
BackendInfo ProbeWaylandCapture()     { return NotImplemented("wayland-pipewire"); }
BackendInfo ProbeWaylandPresenter()   { return NotImplemented("egl-wayland"); }
BackendInfo ProbeEglX11Presenter()    { return NotImplemented("egl-x11"); }
BackendInfo ProbeNvencLinuxRuntime()  { return NotImplemented("nvenc-linux"); }
BackendInfo ProbeNvdecLinuxRuntime()  { return NotImplemented("nvdec-linux"); }
BackendInfo ProbeWgc()                { return NotImplemented("wgc"); }
BackendInfo ProbeNvencWindows()       { return NotImplemented("nvenc"); }
BackendInfo ProbeAmfRuntime()         { return NotImplemented("amf"); }
BackendInfo ProbeOneVplRuntime()      { return NotImplemented("onevpl"); }
BackendInfo ProbeD3d11va()            { return NotImplemented("d3d11va"); }
BackendInfo ProbeDxgiFlipPresenter()  { return NotImplemented("dxgi-flip"); }
std::vector<AdapterInfo> EnumerateD3D11Adapters() { return {}; }
#endif

SessionType DetectSessionType() {
#if defined(_WIN32)
    return SessionType::WindowsDesktop;
#else
    // Linux: XDG_SESSION_TYPE is the compositor's own answer;
    // fall back to WAYLAND_DISPLAY / DISPLAY env if it's unset
    // (some login managers don't export XDG_SESSION_TYPE).
    if (const char* st = std::getenv("XDG_SESSION_TYPE"); st && *st) {
        if (std::string(st) == "wayland") return SessionType::LinuxWayland;
        if (std::string(st) == "x11")     return SessionType::LinuxX11;
    }
    if (const char* w = std::getenv("WAYLAND_DISPLAY"); w && *w) {
        return SessionType::LinuxWayland;
    }
    if (const char* d = std::getenv("DISPLAY"); d && *d) {
        return SessionType::LinuxX11;
    }
    return SessionType::Unknown;
#endif
}

// ── Streaming-block synthesiser ──────────────────────────────
//
// Collapses the per-backend probe results into the four-field
// streaming block the Python UI surfaces. Canonical user-facing
// message comes from issue #23's 2026-04-22 decision — shipped
// verbatim so every code path that reaches a console ends at the
// same text.

namespace {

constexpr const char* kNoEncoderMessage =
    "Display streaming requires a GPU with built-in H.264 video "
    "encoding \xe2\x80\x94 Intel Quick Sync (typically 2013 or "
    "newer), NVIDIA NVENC (typically 2012 or newer), or AMD VCN "
    "(typically 2017 or newer). Your current hardware doesn't "
    "have one. The rest of UnIO still works on this machine.";

constexpr const char* kNoCaptureMessage =
    "Display streaming requires a supported screen-capture "
    "backend. On Linux this means X11 (XComposite) or Wayland "
    "(xdg-desktop-portal + PipeWire); on Windows it means WGC "
    "(Windows 10 build 17134 or newer). None is available on "
    "this host. The rest of UnIO still works on this machine.";

constexpr const char* kProbeErrorMessage =
    "Display streaming is unavailable because the capability "
    "probe failed to complete. Check the helper log for the "
    "specific backend errors. The rest of UnIO still works "
    "on this machine.";

bool AnyH264Encoder(const std::vector<BackendInfo>& encoders) {
    for (const auto& e : encoders) {
        if (!e.available) continue;
        for (const auto& c : e.codecs) {
            if (c == "h264") return true;
        }
    }
    return false;
}

bool AnyCapture(const std::vector<BackendInfo>& captures) {
    for (const auto& c : captures) {
        if (c.available) return true;
    }
    return false;
}

bool AnyAvailable(const std::vector<BackendInfo>& items) {
    for (const auto& b : items) {
        if (b.available) return true;
    }
    return false;
}

bool AnyH264Decoder(const std::vector<BackendInfo>& decoders) {
    for (const auto& d : decoders) {
        if (!d.available) continue;
        for (const auto& c : d.codecs) {
            if (c == "h264") return true;
        }
    }
    return false;
}

}  // namespace

StreamingInfo BuildStreamingBlock(const ProbeResult& result) {
    StreamingInfo s;

    // Surface every GPU the probe saw, even when streaming is
    // unavailable. Support requests benefit from seeing "your
    // probe detected an Intel HD Graphics 3000" in the output.
    s.detected_gpus.reserve(result.adapters.size());
    for (const auto& a : result.adapters) {
        std::string entry = a.vendor;
        if (!a.name.empty() && a.name != a.vendor) {
            entry += " / " + a.name;
        }
        s.detected_gpus.push_back(std::move(entry));
    }

    const bool has_capture   = AnyCapture(result.captures);
    const bool has_encoder   = AnyH264Encoder(result.encoders);
    const bool has_decoder   = AnyH264Decoder(result.decoders);
    const bool has_presenter = AnyAvailable(result.presenters);

    if (has_capture && has_encoder && has_decoder && has_presenter) {
        s.available = true;
        s.reason = StreamingReason::Available;
        s.user_message.clear();
        return s;
    }
    s.available = false;
    // Reason ordering: encoder / decoder first (same
    // hardware-landscape problem, same canonical message per
    // issue #23). Then capture / presenter.
    //
    // UnIO is symmetric — every PC is both source and sink —
    // so "streaming available" means all four roles light up,
    // and any missing role refuses both start_outbound and
    // start_inbound with one consistent message.
    if (!has_encoder || !has_decoder) {
        s.reason = StreamingReason::NoHwEncoder;
        s.user_message = kNoEncoderMessage;
    } else if (!has_capture || !has_presenter) {
        s.reason = StreamingReason::NoCapture;
        s.user_message = kNoCaptureMessage;
    } else {
        s.reason = StreamingReason::ProbeError;
        s.user_message = kProbeErrorMessage;
    }
    return s;
}

// ── Top-level runner — body lands across commits 2 + 3 ────────

#if defined(__linux__)
// Read /sys/class/drm/card*/device/{vendor,device} to enumerate
// GPU adapters on Linux. Cheap — just a few dozen file reads.
// vendor is reported as an 0x-prefixed PCI id, which we map to
// a human name via VendorFromPciId.
static std::vector<AdapterInfo> EnumerateLinuxAdapters() {
    std::vector<AdapterInfo> out;
    DIR* dir = ::opendir("/sys/class/drm");
    if (!dir) return out;
    struct dirent* ent;
    while ((ent = ::readdir(dir)) != nullptr) {
        std::string name(ent->d_name);
        if (name.rfind("card", 0) != 0) continue;
        // Skip card0-DP-1 etc. — only the bare cardN entries
        // point at a physical adapter.
        if (name.find('-') != std::string::npos) continue;
        const std::string base =
            std::string("/sys/class/drm/") + name + "/device/";
        const std::string vendor = ReadFirstLine(base + "vendor");
        if (vendor.empty()) continue;
        AdapterInfo ai;
        ai.vendor = VendorFromPciId(vendor);
        ai.name = ai.vendor + " " +
                   ReadFirstLine(base + "device");
        // Driver name (modalias-derived) — best-effort.
        const std::string uevent = ReadFirstLine(base + "uevent");
        (void)uevent;
        out.push_back(std::move(ai));
    }
    ::closedir(dir);
    return out;
}
#endif

namespace {

bool EnvFlagSet(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    // Anything that isn't an explicit "0" / "false" / "no" is
    // considered set. This matches the usual shell convention
    // and matches how stream_manager's other env knobs behave.
    if (std::strcmp(v, "0") == 0) return false;
    if (std::strcmp(v, "false") == 0) return false;
    if (std::strcmp(v, "no") == 0) return false;
    return true;
}

// Short-circuit result emitted when UNIO_PIPE_DISABLE_PROBE=1.
// Falls back to the old hardwired-per-OS behaviour: claims every
// per-OS backend is available, emits a benign streaming block.
// Regression kill-switch only — not a runtime-normal code path.
ProbeResult BuildDisabledProbe() {
    ProbeResult r;
    r.probe_disabled = true;
    r.session = DetectSessionType();

    auto hardwired = [](const char* name, std::initializer_list<const char*> codecs) {
        BackendInfo b;
        b.name = name;
        b.available = true;
        for (auto* c : codecs) b.codecs.emplace_back(c);
        b.notes = "hardwired (UNIO_PIPE_DISABLE_PROBE=1)";
        return b;
    };

#if defined(__linux__)
    r.captures.push_back(hardwired("xcomposite", {"bgra"}));
    r.encoders.push_back(hardwired("vaapi", {"h264"}));
    r.decoders.push_back(hardwired("vaapi", {"h264"}));
    r.presenters.push_back(hardwired("egl-x11", {"nv12-dmabuf"}));
#elif defined(_WIN32)
    r.captures.push_back(hardwired("wgc", {"bgra"}));
    r.encoders.push_back(hardwired("nvenc", {"h264"}));
    r.decoders.push_back(hardwired("d3d11va", {"h264"}));
    r.presenters.push_back(hardwired("dxgi-flip", {"nv12"}));
#endif

    r.streaming.available = true;
    r.streaming.reason = StreamingReason::Available;
    return r;
}

ProbeResult BuildForceNoStreamingProbe() {
    ProbeResult r = BuildDisabledProbe();
// probe_disabled reads as "skipped real probes, using hardwired
    // defaults." The force-no-streaming path is semantically
    // different — it's a test-mode forcing a refusal, not a
    // default-fallback. Flip the flag so consumers that key on
    // probe_disabled to detect "no useful info" don't get misled
    // (per PR #33 review). The streaming block itself is the
    // authoritative signal in either case.
    r.probe_disabled = false;
    // Clear the hardwired happy-path backends so the streaming
    // block honestly reflects "forced unavailable."
    r.encoders.clear();
    r.decoders.clear();
    r.streaming = BuildStreamingBlock(r);
    return r;
}

ProbeResult DoRealProbe() {
    ProbeResult r;
    r.probe_disabled = false;
    r.session = DetectSessionType();

#if defined(__linux__)
    r.adapters = EnumerateLinuxAdapters();

    // Capture paths. X11 and Wayland both probe; the active
    // session picks which one the runner advertises as the
    // default. We keep both in the list even when one is
    // unavailable so the audit trail shows *why*.
    r.captures.push_back(ProbeXComposite());
    r.captures.push_back(ProbeWaylandCapture());

    // Codec runtimes. VA-API lights up on Intel + AMD + NVIDIA's
    // old (pre-proprietary) driver; NVENC / NVDEC light up on
    // the proprietary NVIDIA driver.
    const BackendInfo vaapi = ProbeVaapi();
    if (vaapi.available) {
        BackendInfo enc = vaapi;
        enc.name = "vaapi";
        r.encoders.push_back(enc);
        BackendInfo dec = vaapi;
        dec.name = "vaapi";
        r.decoders.push_back(dec);
    } else {
        BackendInfo unavail;
        unavail.name = "vaapi";
        unavail.reason = vaapi.reason;
        r.encoders.push_back(unavail);
        unavail.name = "vaapi";
        r.decoders.push_back(unavail);
    }
    r.encoders.push_back(ProbeNvencLinuxRuntime());
    r.decoders.push_back(ProbeNvdecLinuxRuntime());

    // Presenters.
    r.presenters.push_back(ProbeEglX11Presenter());
    r.presenters.push_back(ProbeWaylandPresenter());
#elif defined(_WIN32)
    // Windows probes land in commit 3.
    r.adapters = EnumerateD3D11Adapters();
    r.captures.push_back(ProbeWgc());
    r.encoders.push_back(ProbeNvencWindows());
    r.encoders.push_back(ProbeAmfRuntime());
    r.encoders.push_back(ProbeOneVplRuntime());
    r.decoders.push_back(ProbeD3d11va());
    r.presenters.push_back(ProbeDxgiFlipPresenter());
#endif

    r.streaming = BuildStreamingBlock(r);
    return r;
}

}  // namespace (probe internals)

// ── Public entry point ──────────────────────────────────────────
//
// Cached single-call — once per process. The real probes are
// side-effect-safe (they open + close every resource they touch)
// but some of them cost 5-20 ms, which adds up when helper_caps
// and start_outbound / start_inbound each call ProbeAll().
// Callers that need a fresh probe re-launch the helper; in
// practice the hardware landscape doesn't change during a
// session, so a one-time cache matches reality.
//
// Env overrides (in priority order):
//   UNIO_PIPE_FORCE_NO_STREAMING=1  — force streaming.available
//       to false with the canonical no-encoder message, for
//       testing the UI refusal path without actual no-GPU hw.
//   UNIO_PIPE_DISABLE_PROBE=1       — skip all real probes,
//       return a hardwired per-OS happy-path result flagged
//       with probe_disabled=true. Regression kill-switch.
ProbeResult ProbeAll() {
    static ProbeResult cached;
    static std::once_flag flag;
    std::call_once(flag, []() {
        if (EnvFlagSet("UNIO_PIPE_FORCE_NO_STREAMING")) {
            cached = BuildForceNoStreamingProbe();
            std::fprintf(stderr,
                "unio-pipe: probe forced to NoStreaming "
                "(UNIO_PIPE_FORCE_NO_STREAMING=1)\n");
            return;
        }
        if (EnvFlagSet("UNIO_PIPE_DISABLE_PROBE")) {
            cached = BuildDisabledProbe();
            std::fprintf(stderr,
                "unio-pipe: probe disabled, using hardwired "
                "per-OS defaults (UNIO_PIPE_DISABLE_PROBE=1)\n");
            return;
        }
        cached = DoRealProbe();
    });
    return cached;
}

}  // namespace unio
