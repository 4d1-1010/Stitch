/*! @file image_loader.hpp
 *  @brief Decode an in-memory PNG to RGBA. Thin wrapper over
 *         stb_image, kept off the public theme API so only the
 *         platform TU sees the stb headers.
 */
#pragma once

#include <cstddef>

namespace unio_ui {

/// RGBA8 pixel buffer + dimensions. `pixels` is owned by the
/// caller; free with @ref free_decoded_image when done.
struct DecodedImage {
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
};

/// Decode a PNG (or JPEG / BMP — stb_image handles them all) to
/// 4-channel RGBA8. Returns `pixels=nullptr` on failure.
DecodedImage decode_image(const unsigned char* data, std::size_t size);

/// Free pixels allocated by @ref decode_image.
void free_decoded_image(DecodedImage& img);

}  // namespace unio_ui
