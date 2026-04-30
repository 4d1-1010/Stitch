/// @file image_loader.cpp
/// @brief @c stb_image wrapper producing RGBA8 pixel buffers.

#include "util/image_loader.hpp"

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

namespace xorio {

DecodedImage decode_image(const unsigned char* data, std::size_t size) {
    int w = 0, h = 0, channels = 0;
    unsigned char* px = stbi_load_from_memory(
        data, static_cast<int>(size), &w, &h, &channels, 4);
    return {px, w, h};
}

void free_decoded_image(DecodedImage& img) {
    if (img.pixels) {
        stbi_image_free(img.pixels);
        img.pixels = nullptr;
    }
    img.width = img.height = 0;
}

}  // namespace xorio
