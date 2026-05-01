/// @file stream_window.cpp
/// @brief No-op @ref xorio::platform::StreamWindow implementation.

#include "platform/stream_window.hpp"

#include <cstdio>

namespace xorio::platform {

namespace {

/// @brief Placeholder that accepts frames but presents nothing.
///
/// @todo Replace with a native sink window once @c unio-pipe
/// exposes decoded-frame textures through the orchestrator layer.
class StubStreamWindow final : public StreamWindow {
public:
    void push_frame(ImTextureID /*texture*/) override {}
};

}  // namespace

std::unique_ptr<StreamWindow>
open_stream_window(const StreamWindowConfig& cfg) {
    std::fprintf(stderr,
                 "[stream_window] stub: would open at %dx%d+%d+%d src=%s\n",
                 cfg.width, cfg.height, cfg.x, cfg.y,
                 cfg.source_label.c_str());
    return std::make_unique<StubStreamWindow>();
}

}  // namespace xorio::platform
