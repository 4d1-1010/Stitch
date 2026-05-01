/// @file source_overlay.cpp
/// @brief No-op @ref xorio::platform::SourceOverlay implementation.

#include "platform/source_overlay.hpp"

#include <cstdio>

namespace xorio::platform {

namespace {

/// @brief Placeholder that reports creation but opens no window.
///
/// @todo Replace with native platform code (WDA_EXCLUDEFROMCAPTURE
/// on Windows, XORIO_CAPTURE_EXCLUDE atom on X11) once the
/// orchestrator starts driving outgoing streams.
class StubSourceOverlay final : public SourceOverlay {};

}  // namespace

std::unique_ptr<SourceOverlay>
open_source_overlay(const SourceOverlayConfig& cfg) {
    std::fprintf(stderr,
                 "[source_overlay] stub: would open at %dx%d+%d+%d "
                 "src=%s dst=%s\n",
                 cfg.width, cfg.height, cfg.x, cfg.y,
                 cfg.source_label.c_str(),
                 cfg.destination_label.c_str());
    return std::make_unique<StubSourceOverlay>();
}

}  // namespace xorio::platform
