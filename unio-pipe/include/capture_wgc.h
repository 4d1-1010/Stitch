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

    bool Open();                          // init WinRT + D3D11
    using FrameCallback = void(*)(CpuFramePtr, void*);
    bool Start(WgcRect rect, int fps,
               FrameCallback cb, void* user);
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace unio
