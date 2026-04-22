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
    b.notes = "libcuda + libnvidia-encode loadable; session-open test deferred to #27";
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
    b.notes = "libcuda + libnvcuvid loadable; parser init deferred to #21";
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

// ── Streaming-block synthesiser — body lands in commit 4 ──────

StreamingInfo BuildStreamingBlock(const ProbeResult& result) {
    StreamingInfo s;
    (void)result;
    s.available = false;
    s.reason = StreamingReason::ProbeError;
    s.user_message = "(probe not yet implemented)";
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

ProbeResult ProbeAll() {
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

}  // namespace unio
