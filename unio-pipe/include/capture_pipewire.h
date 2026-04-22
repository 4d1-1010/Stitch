#pragma once

#if !defined(__linux__)
#error "capture_pipewire.h is Linux-only"
#endif

#include "frame.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace unio {

// PipeWire / xdg-desktop-Portal capture backend for Wayland.
//
// Captures the screen via the standard Wayland screen-capture stack:
//
//   1. sdbus-c++ calls xdg-desktop-portal ScreenCast API to create
//      a capture session and obtain a PipeWire file descriptor.
//   2. libpipewire connects to the daemon via the fd, creates a
//      stream proxy for the screen-cast node.
//   3. Each frame is a BGRA buffer pushed into the SpscRing<CpuFrame>
//      via the FrameCallback.
//
// Dependencies:
//   - libpipewire-0.3 (PipeWire client)
//   - libspa-0.2 (stream protocol abstraction)
//   - sdbus-c++ (D-Bus for portal communication)
//   - xdg-desktop-portal with ScreenCast support (runtime)
//
// This is the Wayland counterpart of XCompositeCapture (X11) and
// WgcCapture (Windows). Same threading model: runs on its own
// capture thread, frames fire at ``fps`` cadence.

// Local copy of the capture rect — same 16-byte POD as
// capture_xcomposite.h::CaptureRect. The three capture headers
// never compile in the same TU, so duplicating is cheaper than
// a shared header for four ints.
struct PipewireRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

class PipeWireCapture {
public:
    PipeWireCapture();
    ~PipeWireCapture();

    // Opens the D-Bus connection, creates a ScreenCast session via
    // xdg-desktop-portal, and connects to PipeWire. Returns false
    // on any failure (no Wayland session, no PipeWire daemon, etc.).
    bool Open();

    using FrameCallback = void(*)(CpuFramePtr, void*);

    // Start the capture thread. Frames fire at ``fps`` (up to fps —
    // the loop polls the PipeWire stream and skips ticks when no
    // new buffer is available). Stops on Close() or destruction.
    bool Start(PipewireRect rect, int fps,
               FrameCallback cb, void* user);

    // Stop + join the capture thread. Safe to call multiple times.
    void Close();

    // Internal implementation — public so file-scope callbacks
    // in capture_pipewire.cpp can access it.
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace unio