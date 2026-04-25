/// @file token.hpp
/// @brief Vendor-signed licence token — fields the user sees plus
/// the signature that proves vendor origin.
#pragma once

#include "license/tier.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace unio_ui::license {

/// @brief Parsed, unverified licence payload.
///
/// Produced by @c codec::decode from pasted bytes. Signature
/// verification is a separate step (@c license::verifier).
struct LicenseToken {
    std::uint32_t              schema_version   = 1;
    std::array<std::uint8_t, 16> license_id{};   ///< UUID, unique per issuance.
    std::string                customer;        ///< User identifier (email, handle).
    Tier                       tier             = Tier::Trial;
    std::uint64_t              issued_at_ns     = 0; ///< Vendor wall clock at sign time.
    std::uint64_t              valid_from_ns    = 0; ///< Usually == @c issued_at_ns.
    std::uint64_t              duration_seconds = 0; ///< Convenience; must match interval.
    std::uint64_t              expires_at_ns    = 0; ///< @c valid_from_ns + duration_seconds × 1e9.
    std::uint32_t              issuer_key_id    = 0; ///< Identifies which vendor key signed.
};

/// @brief Token plus the vendor signature that covers the
/// serialised payload.
struct SignedLicenseToken {
    LicenseToken                 token;
    std::array<std::uint8_t, 64> signature{};
};

}  // namespace unio_ui::license
