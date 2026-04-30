/// @file machine_color.cpp
/// @brief Pure deterministic mapping `machine_id → ImVec4`.
///
/// CRC32-of-id seeds three biased ranges (hue / saturation /
/// lightness) that always sit inside the brand's lilac family.
/// Same id → same colour, regardless of which screen renders it.

#include "ui/machine_color.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace xorio_ui::ui {

namespace {

/// @brief Hue slice centred on the lilac brand accent.
constexpr float kLilacHue = 252.0f / 360.0f;
constexpr float kHueSpan  = 0.18f;
constexpr float kSatMin   = 0.55f;
constexpr float kSatMax   = 0.85f;
constexpr float kLightMin = 0.62f;
constexpr float kLightMax = 0.76f;

/// @brief CRC-32/IEEE on a byte string. Reflected polynomial
/// 0xEDB88320, init 0xFFFFFFFF, final XOR with 0xFFFFFFFF.
std::uint32_t crc32_ieee(const std::string& s) {
    static const std::array<std::uint32_t, 256> kTable = []() {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        crc = kTable[(crc ^ b) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/// @brief HLS (0..1) → RGB (0..1).
void hls_to_rgb(float h, float l, float s, float& r, float& g, float& b) {
    if (s == 0.0f) { r = g = b = l; return; }
    auto hue = [](float p, float q, float t) {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 1.0f / 2.0f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };
    const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
    const float p = 2.0f * l - q;
    r = hue(p, q, h + 1.0f / 3.0f);
    g = hue(p, q, h);
    b = hue(p, q, h - 1.0f / 3.0f);
}

}  // namespace

ImVec4 machine_color(const std::string& machine_id) {
    const std::uint32_t seed = crc32_ieee(machine_id);
    const float h_part = static_cast<float>((seed >> 20) & 0xFFFu) / float(0xFFFu);
    const float s_part = static_cast<float>((seed >> 10) & 0x3FFu) / float(0x3FFu);
    const float l_part = static_cast<float>( seed        & 0x3FFu) / float(0x3FFu);

    const float h = std::fmod(kLilacHue + (h_part - 0.5f) * kHueSpan + 1.0f, 1.0f);
    const float s = kSatMin + s_part * (kSatMax - kSatMin);
    const float l = kLightMin + l_part * (kLightMax - kLightMin);

    float r, g, b;
    hls_to_rgb(h, l, s, r, g, b);
    return ImVec4(r, g, b, 1.0f);
}

}  // namespace xorio_ui::ui
