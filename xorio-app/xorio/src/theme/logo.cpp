/// @file logo.cpp
/// @brief Storage for the app-logo GPU texture handle.

#include "theme/logo.hpp"

namespace xorio::theme {

namespace {
LogoTexture g_logo{};
}  // namespace

void register_logo_texture(ImTextureID tex, int w, int h) {
    g_logo = {tex, w, h};
}

const LogoTexture& logo_texture() { return g_logo; }

}  // namespace xorio::theme
