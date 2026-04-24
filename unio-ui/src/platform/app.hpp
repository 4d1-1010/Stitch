/*! @file app.hpp
 *  @brief Common platform interface for unio-ui.
 *
 *  One implementation per OS (`src/platform/win32/`,
 *  `src/platform/x11/`, eventually `src/platform/macos/`). The
 *  entry point in `src/main.cpp` calls @ref
 *  unio_ui::platform::run which hands control to the OS-specific
 *  event loop.
 */
#pragma once

#include <cstdint>
#include <string>

namespace unio_ui::platform {

/// Startup configuration for @ref run.
struct AppConfig {
    std::string window_title = "UnIO";  ///< UTF-8 window title.
    std::int32_t window_width = 1280;   ///< Initial window width in px.
    std::int32_t window_height = 800;   ///< Initial window height in px.
    std::int32_t min_width = 920;       ///< Minimum window width in px.
    std::int32_t min_height = 560;      ///< Minimum window height in px.
};

/*! @brief Run the UI event loop until the user closes the window.
 *  @param cfg Window + startup configuration.
 *  @return Process exit code. Non-zero if window/context setup
 *          failed without ever opening a window.
 */
int run(const AppConfig& cfg);

}  // namespace unio_ui::platform
