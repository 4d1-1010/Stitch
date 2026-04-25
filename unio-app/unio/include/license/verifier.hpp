/// @file verifier.hpp
/// @brief Cryptographic + structural validation of pasted licence
/// tokens.
///
/// Verifies that a wrapped token (`LIC1` + base64) parses cleanly,
/// names a known vendor key, and carries a valid Ed25519 signature
/// from that key. Time-window enforcement (expiry, future-dating)
/// is **not** done here — `LicenseManager` owns wall-clock checks
/// and cross-peer clock-tampering detection.
#pragma once

#include "license/token.hpp"

#include <cstdint>
#include <string>

namespace unio_ui::license {

/// @brief Granular reason a token was rejected.
enum class VerifyError {
    Ok,            ///< Signature valid + token well-formed.
    BadFormat,     ///< Wrap prefix missing / base64 invalid / too short.
    BadPayload,    ///< Wrapped bytes parsed but payload decode failed.
    UnknownIssuer, ///< @c issuer_key_id != any embedded vendor key.
    BadSignature,  ///< EVP_DigestVerify rejected the signature.
};

/// @brief Successful verification populates @c signed_token; on
/// failure the token is left default-constructed.
struct VerifyResult {
    VerifyError        error = VerifyError::BadFormat;
    SignedLicenseToken signed_token;

    explicit operator bool() const noexcept {
        return error == VerifyError::Ok;
    }
};

/// @brief Identifier of the only vendor key currently embedded in
/// the binary. The token's @c issuer_key_id field must match this
/// value. A future commit may grow this to a list when key rotation
/// requires multiple roots co-existing across releases.
inline constexpr std::uint32_t kVendorKeyId = 0;

/// @brief Verify a paste-friendly wrapped token (e.g. the output
/// of `unio-license-tool`). Pure function — no side effects, no
/// allocations beyond the parsed token.
VerifyResult verify_wrapped_token(const std::string& wrapped);

}  // namespace unio_ui::license
