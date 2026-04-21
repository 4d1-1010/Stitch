#pragma once

#include "decoder.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace unio {

// Presenter interface. Takes DecodedFrame handles from the decoder
// and paints them on a physical monitor at the sink. One concrete
// implementation per OS:
//  - EGL on X11 (Linux)     — this file's sibling
//  - DXGI flip model        — Windows, PR 6 Day 11
//
// The presenter owns its own display thread; Present() just posts
// a frame reference to a ring and returns immediately so the
// decoder's callback thread never blocks on glXSwapBuffers.
//
// Latency budget per the scope memo is ≤16 ms GPU glass-to-glass
// at 60 Hz, so the presenter must use SyncInterval=0 / immediate
// flip / tear-present — a full-frame wait makes the budget
// physically unachievable.
class Presenter {
public:
    virtual ~Presenter() = default;

    struct Config {
        // Output geometry. On Linux we use an override-redirect
        // X11 window at this rect; on Windows a borderless
        // top-level. (0,0,0,0) means "full primary monitor".
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        // Title used only for debug / taskbar identification.
        std::string window_title = "unio-pipe";
    };

    virtual std::optional<std::string> Init(const Config& cfg) = 0;

    // Hand over a decoded frame. Ownership semantics match the
    // DecodedFrame contract — surface_handle is borrowed, the
    // presenter must finish using it before returning (or copy
    // the pixel data out). In practice the decoder keeps the
    // surface around long enough via its own DPB slot.
    virtual void Present(const DecodedFrame& frame) = 0;

    // Counters exposed via helper_status.
    virtual std::uint64_t FramesPresented() const = 0;

    virtual std::string_view Name() const = 0;
};

std::unique_ptr<Presenter> MakeEglX11Presenter();

}  // namespace unio
