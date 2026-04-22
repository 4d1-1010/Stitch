#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Runtime capability probe (WP 10 / #22). One per helper startup,
// re-probe available via the helper_recaps RPC. Enumerates
// captures / encoders / decoders / presenters + the GPU adapters
// underneath, and collapses the result into a streaming-summary
// block the Python side surfaces in UI (WP 8). See issue #22 + #14
// on 4d1-1010/Stitch for scope; individual probe implementations
// are reused by the vendor sub-issues (#7 Wayland capture, #21
// NVDEC-Linux, #25 AMF, #26 oneVPL, #27 NVENC-Linux, #30
// Wayland presenter).

namespace unio {

enum class SessionType {
    Unknown,
    WindowsDesktop,
    LinuxX11,
    LinuxWayland,
};

// One entry per capture / encoder / decoder / presenter backend.
// available=false entries carry a reason so support requests can
// explain *why* a backend didn't light up. Never removed from the
// output — an unavailable backend is still information worth
// surfacing to the user.
struct BackendInfo {
    std::string name;                // e.g. "vaapi", "nvenc", "wgc"
    bool available = false;
    std::vector<std::string> codecs; // e.g. {"h264-cbp", "h264-main"}
    std::string max_resolution;      // "1920x1080" or "" if unknown
    std::string reason;              // filled when available=false
    std::string notes;               // free-form audit note (e.g. driver version)
};

// One per adapter the probe saw. Even unavailable encoders list
// their host adapter so bug reports can see "your probe detected
// an Intel HD Graphics 3000 but no Quick Sync session would open."
struct AdapterInfo {
    std::string name;                // "NVIDIA GeForce GTX 1650 Ti" etc.
    std::string vendor;              // "NVIDIA" / "AMD" / "Intel" / "Microsoft Basic"
    std::string driver_version;      // empty when unknown
    std::string luid;                // hex-serialised DXGI LUID on Windows, else ""
};

// Canonical refusal reasons. Serialised as snake_case strings in
// the helper_caps JSON so the Python side can switch on them
// without parsing user_message prose.
enum class StreamingReason {
    Available,
    NoHwEncoder,
    NoCapture,
    ProbeError,
};

const char* StreamingReasonName(StreamingReason r);

// Top-level streaming summary the Python UI surfaces. `available`
// is true iff at least one capture AND one hardware H.264 encoder
// AND one decoder-compatible path exist on this host. When false,
// `user_message` carries the canonical user-facing text shipped
// verbatim from the helper — decided 2026-04-22 on issue #23.
struct StreamingInfo {
    bool available = false;
    StreamingReason reason = StreamingReason::ProbeError;
    std::vector<std::string> detected_gpus;
    std::string user_message;
};

// Full probe snapshot. Returned by ProbeAll() and carried by the
// helper_caps RPC response (alongside the legacy HelperCaps shape
// in unio_pipe.h for a release-cycle's back-compat).
struct ProbeResult {
    SessionType session = SessionType::Unknown;
    std::vector<AdapterInfo> adapters;
    std::vector<BackendInfo> captures;
    std::vector<BackendInfo> encoders;
    std::vector<BackendInfo> decoders;
    std::vector<BackendInfo> presenters;
    StreamingInfo streaming;
    bool probe_disabled = false;     // true when UNIO_PIPE_DISABLE_PROBE=1
};

// Single entry point — called once per process, cached via
// std::call_once. Safe to call from any thread after startup;
// the body is idempotent.
//
// Cost: ~5-20 ms on a typical host. Linux probes open +
// vaInitialize a VA display and close it, plus a few dlopen
// probes and an EGL init/terminate on the X display. Windows
// probes do LoadLibrary + GetProcAddress for each vendor
// runtime, plus one D3D11CreateDevice(VIDEO) + QI + release.
// Actual hardware-session-open (which would detect driver
// misconfigurations beyond DLL presence) is deferred to the
// vendor sub-issues of WP 10 (#21, #25, #26, #27) — those
// produce a more accurate "usable" signal at higher probe
// cost.
ProbeResult ProbeAll();

// Per-backend probe entry points. Exposed so the vendor sub-issues
// (#7 / #21 / #25 / #26 / #27 / #30) can extend their own Probe*
// without touching the runner. Each returns a fully-populated
// BackendInfo (including available=false + reason on failure).
BackendInfo ProbeVaapi();
BackendInfo ProbeXComposite();
BackendInfo ProbeWaylandCapture();
BackendInfo ProbeWaylandPresenter();
BackendInfo ProbeEglX11Presenter();
BackendInfo ProbeNvencLinuxRuntime();
BackendInfo ProbeNvdecLinuxRuntime();

BackendInfo ProbeWgc();
BackendInfo ProbeNvencWindows();
BackendInfo ProbeAmfRuntime();
BackendInfo ProbeOneVplRuntime();
BackendInfo ProbeD3d11va();
BackendInfo ProbeDxgiFlipPresenter();

std::vector<AdapterInfo> EnumerateD3D11Adapters();
SessionType DetectSessionType();

// Builds the streaming block from a populated ProbeResult. Pure
// function — no I/O, no side effects — so the negotiation logic
// in #24 can re-run it on a hypothetical probe result (e.g. to
// preview what a peer's probe would conclude).
StreamingInfo BuildStreamingBlock(const ProbeResult& result);

}  // namespace unio
