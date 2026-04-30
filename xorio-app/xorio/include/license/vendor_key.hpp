/// @file vendor_key.hpp
/// @brief Accessor for the vendor Ed25519 public key embedded in
/// the binary. Consumed by the licence verifier.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace xorio_ui::license {

/// @brief Return the raw 32-byte Ed25519 vendor public key.
///
/// The key is generated out-of-band (see @c dev/README.md) and
/// embedded via @c cmake/EmbedBinary.cmake; this accessor is the
/// only public path to its bytes.
const std::array<std::uint8_t, 32>& vendor_public_key();

}  // namespace xorio_ui::license
