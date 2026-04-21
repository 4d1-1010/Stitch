#pragma once

#if !defined(__linux__)
#error "capture_xcomposite.h is Linux-only"
#endif

#include "frame.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace unio {

// Ported from unio/features/capture_xcomposite.py. Day-2 scope:
// root-window XShmGetImage at the capture rect, same single-shot
// 8 MB host-memory copy the Python path already uses. Per-window
// composite + ancestor-resolution + EWMH stacking come with the
// actual overlay-exclusion work; the Python version is still the
// reference.
//
// Capture runs on its own thread. The consumer passes in an
// SpscRing<CpuFrame> (via a callback) to receive frames.

struct CaptureRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

class XCompositeCapture {
public:
    XCompositeCapture();
    ~XCompositeCapture();

    // Opens the X display, binds the XShm extension, verifies
    // that the root pixmap actually contains real pixels (early
    // GNOME Shell builds left it all-black). Returns false on
    // any of those failing.
    bool Open();

    // Called by the capture thread once per successfully-grabbed
    // frame. Implementations typically push into an SpscRing.
    // Called from the capture thread — do NOT block it.
    using FrameCallback = void(*)(CpuFramePtr, void*);

    // Spawn the capture thread. Frames fire at ``fps`` (actually
    // "up to fps" — the loop polls XDamage if available and
    // skips ticks when nothing changed). Stops on Close() or
    // destruction.
    bool Start(CaptureRect rect, int fps,
               FrameCallback cb, void* user);

    // Stop + join the capture thread. Safe to call multiple
    // times. Called automatically from the destructor.
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace unio
