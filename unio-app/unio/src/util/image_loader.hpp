/// @file image_loader.hpp
/// @brief In-memory PNG → RGBA8 decoder.
#pragma once

#include <cstddef>

namespace unio_ui {

/// @brief Caller-owned RGBA8 pixel buffer.
struct DecodedImage {
    unsigned char* pixels = nullptr;  ///< Tightly-packed RGBA; release via @ref free_decoded_image.
    int width  = 0;
    int height = 0;
};

/// @brief Decode an in-memory image to 4-channel RGBA8.
/// @param data  Source bytes.
/// @param size  Size of @p data in bytes.
/// @return Populated @ref DecodedImage; @c pixels is @c nullptr on failure.
DecodedImage decode_image(const unsigned char* data, std::size_t size);

/// @brief Release pixels allocated by @ref decode_image.
void free_decoded_image(DecodedImage& img);

}  // namespace unio_ui
