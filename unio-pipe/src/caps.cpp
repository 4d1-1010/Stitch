// Capability probe. Day-1 scope: tell the caller what drivers we
// *could* use. Does NOT initialise any GPU context — that costs
// 100s of ms and would block startup. Actual encoder bring-up
// happens per-stream in start_outbound.

#include "unio_pipe.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace unio {

namespace {

#if defined(__linux__)

// /dev/dri/renderD128 being readable is the crudest-but-accurate
// VA-API probe on Linux: the libva dispatcher opens this, and if
// it's not there we aren't going to encode anything regardless
// of what drivers report. Avoids pulling libva just to ask.
bool VaApiAvailable() {
    std::ifstream dri("/dev/dri/renderD128");
    return static_cast<bool>(dri);
}

// NVENC on Linux needs the closed-source nvidia driver. We
// approximate by checking for /dev/nvidia0 existing — same idea
// as the VA-API probe.
bool NvencLinuxAvailable() {
    std::ifstream nv("/dev/nvidia0");
    return static_cast<bool>(nv);
}

#endif  // __linux__

#if defined(_WIN32)

// Real Windows detection is WGC / D3D11CreateDevice / NvEncInit.
// All three need COM apartment + heavy initialisation; we defer
// to per-stream start. Day 1 just lists every vendor blindly;
// start_outbound returns an explicit error if the vendor turns
// out not to be available when we actually try.
bool NvencWindowsAvailable() { return true; }
bool AmfAvailable() { return true; }
bool OneVplAvailable() { return true; }

#endif  // _WIN32

}  // namespace

HelperCaps ProbeCaps() {
    HelperCaps caps;

#if defined(__linux__)
    if (VaApiAvailable()) {
        caps.encoders.emplace_back("vaapi");
        caps.decoders.emplace_back("vaapi");
    }
    if (NvencLinuxAvailable()) {
        // Commented in README: Linux NVENC + NVDEC is PR 7/8
        // scope. Advertise it so the Python side can plan; actual
        // start_outbound still fails loud if the driver isn't
        // cooperative at runtime.
        caps.encoders.emplace_back("nvenc");
        caps.decoders.emplace_back("nvdec");
    }
    caps.presenters.emplace_back("egl");
#elif defined(_WIN32)
    if (NvencWindowsAvailable()) {
        caps.encoders.emplace_back("nvenc");
        caps.decoders.emplace_back("d3d11va");
    }
    if (AmfAvailable()) {
        caps.encoders.emplace_back("amf");
    }
    if (OneVplAvailable()) {
        caps.encoders.emplace_back("onevpl");
    }
    caps.presenters.emplace_back("dxgi_flip");
#endif

    return caps;
}

}  // namespace unio
