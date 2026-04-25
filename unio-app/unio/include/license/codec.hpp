/// @file codec.hpp
/// @brief Binary + base64 serialisation for @ref LicenseToken
/// plus its signature.
///
/// Shared between the `unio-ui` binary (which decodes + verifies
/// pasted tokens) and the `unio-license-tool` CLI (which encodes
/// + signs tokens). Both TUs link @c license/codec.cpp.
///
/// Binary layout, little-endian:
///
///   [u32 magic = 'UNLO']
///   [u32 schema_version]
///   [u8[16] license_id]
///   [u32 customer_len] [u8 × customer_len customer]
///   [u32 tier]
///   [u64 issued_at_ns]
///   [u64 valid_from_ns]
///   [u64 duration_seconds]
///   [u64 expires_at_ns]
///   [u32 issuer_key_id]
///
/// Pasted token is @c base64("LIC1" + payload + signature).
#pragma once

#include "license/token.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace unio_ui::license::codec {

/// @brief Magic prefix stamped on the payload bytes.
inline constexpr std::uint32_t kPayloadMagic = 0x4F4C4E55u;  // "UNLO"

/// @brief Magic prefix on base64-wrapped tokens.
inline constexpr const char* kWrapPrefix = "LIC1";

/// @brief Serialise a token to the canonical byte layout that is
/// signed + verified. Deterministic; identical input always
/// produces identical bytes.
std::vector<std::uint8_t> encode_payload(const LicenseToken& token);

/// @brief Parse bytes produced by @ref encode_payload back into a
/// token. Returns @c std::nullopt on malformed input.
std::optional<LicenseToken>
decode_payload(const std::uint8_t* bytes, std::size_t len);

/// @brief Wrap payload + signature into a paste-friendly token
/// string (@c kWrapPrefix + base64).
std::string
encode_wrapped(const std::vector<std::uint8_t>& payload,
               const std::array<std::uint8_t, 64>& signature);

/// @brief Unwrap a pasted token string back into payload + signature.
/// Returns @c std::nullopt on malformed input.
std::optional<std::pair<std::vector<std::uint8_t>,
                        std::array<std::uint8_t, 64>>>
decode_wrapped(const std::string& wrapped);

/// @brief Base64 (standard alphabet, no wrapping) encode.
std::string b64_encode(const std::uint8_t* bytes, std::size_t len);

/// @brief Base64 decode. Returns @c std::nullopt on invalid input.
std::optional<std::vector<std::uint8_t>>
b64_decode(const std::string& input);

}  // namespace unio_ui::license::codec
