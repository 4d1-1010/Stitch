#pragma once

#if !defined(_WIN32)
#error "capture_wgc.h is Windows-only"
#endif

#include "frame.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace unio {

// Local copy of the XComposite capture rect — the two files
// never compile in the same translation unit, so duplicating
// the 16-byte POD is cheaper than introducing a third shared
// header for four ints.
struct WgcRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Windows Graphics Capture — the Windows sibling of
// XCompositeCapture. Uses the Win10 1903+ WGC API under the
// hood (GraphicsCaptureItem + Direct3D11CaptureFramePool), reads
// each captured ID3D11Texture2D back to host memory via a
// staging texture so the existing CpuFrame-based encoder
// interface doesn't need to change for the MVP. A zero-copy
// path (register the ID3D11Texture2D directly with NVENC) is a
// post-PR-6 optimisation.
//
// Frames come out at the WGC pool's cadence (display refresh),
// which is our capture rate ceiling. The `fps` argument is
// advisory — the pool delivers what the compositor has, we don't
// fake ticks from it.
//
// Like XCompositeCapture, WgcCapture is single-monitor for Day
// 8b. Multi-monitor + per-window capture come later.
class WgcCapture {
public:
    WgcCapture();
    ~WgcCapture();

    // Initialise WinRT. If `shared_device` is non-null, WGC adopts
    // it (same device as the encoder → the captured
    // ID3D11Texture2D is accessible to NvEncRegisterResource
    // without a cross-device share). If null, WGC creates its own
    // D3D11 device (legacy CpuFrame path). Void* avoids pulling
    // d3d11.h into this header.
    bool Open(void* shared_device = nullptr);
    using FrameCallback = void(*)(CpuFramePtr, void*);
    // PR 7 Day 2: zero-copy GPU callback. The texture pointer
    // is owned by WgcCapture's internal pool and valid only
    // until the callback returns — consumer must either encode
    // synchronously or CopyResource to its own storage.
    using GpuFrameCallback = void(*)(const GpuFrame&, void*);

    bool Start(WgcRect rect, int fps,
               FrameCallback cb, void* user);
    bool StartGpu(WgcRect rect, int fps,
                  GpuFrameCallback cb, void* user);
    void Close();

private:
    struct Impl;
    // shared_ptr so the WGC FrameArrived lambda can hold a
    // weak_ptr to Impl and survive a race between WGC releasing
    // the handler and our destructor resetting D3D11 resources.
    std::shared_ptr<Impl> impl_;

    static bool SetupMonitor(Impl& impl, WgcRect rect);
    static void StartSession(const std::shared_ptr<Impl>& impl);
};

}  // namespace unio
