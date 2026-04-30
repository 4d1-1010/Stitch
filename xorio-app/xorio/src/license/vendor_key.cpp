/// @file vendor_key.cpp
/// @brief Wraps the CMake-generated vendor-pubkey byte array.

#include "license/vendor_key.hpp"

#include "vendor_master_pub_raw.h"

#include <cstring>

namespace xorio_ui::license {

const std::array<std::uint8_t, 32>& vendor_public_key() {
    static const std::array<std::uint8_t, 32> key = [] {
        static_assert(vendor_master_pub_raw_size == 32,
                      "Vendor pubkey must be 32 raw Ed25519 bytes");
        std::array<std::uint8_t, 32> k{};
        std::memcpy(k.data(), vendor_master_pub_raw_data, 32);
        return k;
    }();
    return key;
}

}  // namespace xorio_ui::license
