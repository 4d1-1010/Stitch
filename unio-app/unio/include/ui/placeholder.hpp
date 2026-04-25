/// @file placeholder.hpp
/// @brief Generic "coming soon" centred screen.
#pragma once

namespace unio_ui::ui::placeholder {

/// @brief Render a centred title + description body.
/// @param title  Heading text.
/// @param body   Descriptive paragraph under the heading.
void render(const char* title, const char* body);

}  // namespace unio_ui::ui::placeholder
