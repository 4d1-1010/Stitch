/// @file logo.hpp
/// @brief Registry for the app-logo GPU texture.
#pragma once

#include "imgui.h"

namespace xorio::theme {

/// @brief GPU handle + dimensions of the app logo.
struct LogoTexture {
    ImTextureID texture = 0;
    int width  = 0;
    int height = 0;
};

/// @brief Record the logo texture after the platform uploads it.
/// @param tex  Backend-specific handle cast to @c ImTextureID.
/// @param w    Texture width in pixels.
/// @param h    Texture height in pixels.
void register_logo_texture(ImTextureID tex, int w, int h);

/// @brief Return the currently-registered logo texture.
/// @c texture is 0 until @ref register_logo_texture runs.
const LogoTexture& logo_texture();

}  // namespace xorio::theme
