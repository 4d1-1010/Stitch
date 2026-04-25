/// @file app.hpp
/// @brief Platform-agnostic window / event-loop entry point.
#pragma once

#include <cstdint>
#include <string>

namespace unio_ui::platform {

/// @brief Startup configuration passed to @ref run.
struct AppConfig {
    std::string window_title = "UnIO";
    std::int32_t window_width  = 1280;
    std::int32_t window_height = 800;
    std::int32_t min_width  = 920;
    std::int32_t min_height = 560;
};

/// @brief Run the UI event loop until the user closes the window.
/// @param cfg  Window + startup configuration.
/// @return Process exit code; non-zero on fatal setup failure.
int run(const AppConfig& cfg);

}  // namespace unio_ui::platform
