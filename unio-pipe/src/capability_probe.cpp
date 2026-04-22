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

ProbeResult ProbeAll() {
    ProbeResult r;
    r.probe_disabled = false;
    r.session = DetectSessionType();
    // Per-backend probes land in the next two commits. For this
    // skeleton commit we emit the session type + an empty backend
    // list + a placeholder streaming block so the JSON shape is
    // already correct and the Python side can be written against
    // it in parallel.
    r.streaming = BuildStreamingBlock(r);
    return r;
}

}  // namespace unio
