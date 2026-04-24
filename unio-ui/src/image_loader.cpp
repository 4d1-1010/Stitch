/*! @file image_loader.cpp
 *  @brief PNG decode via vendored stb_image.
 */

#include "image_loader.hpp"

// stb_image ships unused static helpers and a few compile-flag
// warnings our -Werror / /WX setup would trip on. Silence them
// for the TU that includes the implementation; our own code
// below is still subject to the project's strict flags.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4100 4127 4244 4267 4456 4457 4505 4701 4996)
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace unio_ui {

DecodedImage decode_image(const unsigned char* data, std::size_t size) {
    int w = 0, h = 0, channels = 0;
    unsigned char* px = stbi_load_from_memory(
        data, static_cast<int>(size), &w, &h, &channels, /*req=*/4);
    return {px, w, h};
}

void free_decoded_image(DecodedImage& img) {
    if (img.pixels) {
        stbi_image_free(img.pixels);
        img.pixels = nullptr;
    }
    img.width = img.height = 0;
}

}  // namespace unio_ui
