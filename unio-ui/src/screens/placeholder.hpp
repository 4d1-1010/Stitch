/*! @file placeholder.hpp
 *  @brief Coming-soon screen shared by tabs whose real bodies
 *         haven't been ported yet (Settings, Access, Help).
 *
 *  Renders a centred card with a title and a short description
 *  paragraph. The Python equivalents for Settings + Access do
 *  have real content; those will get their own screen files when
 *  the orchestrator gains sign_in / user-config actions. Help in
 *  the Python is already a coming-soon — we just match it.
 */
#pragma once

namespace unio_ui::screens::placeholder {

/// Render a centred "Coming soon" card.
/// @param title Heading text.
/// @param body  Descriptive paragraph under the heading.
void render(const char* title, const char* body);

}  // namespace unio_ui::screens::placeholder
