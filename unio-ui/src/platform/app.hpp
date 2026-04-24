#pragma once

// Common platform interface for unio-ui. One implementation per
// OS (src/platform/win32/, src/platform/x11/, eventually
// src/platform/macos/). The entry point in src/main.cpp calls
// platform::run() which hands control to the OS-specific event
// loop.

#include <cstdint>
#include <string>

namespace unio_ui::platform {

struct AppConfig {
    std::string window_title = "UnIO";
    std::int32_t window_width = 1280;
    std::int32_t window_height = 800;
    std::int32_t min_width = 920;
    std::int32_t min_height = 560;
};

// Blocks until the user closes the window. Returns the process
// exit code. Any fatal setup error is logged and returns non-zero
// without ever opening a window.
int run(const AppConfig& cfg);

}  // namespace unio_ui::platform
