/// @file crypto.hpp
/// @brief Cryptographic primitives exposed to the rest of the
/// orchestrator (keys, signatures, signed records).
///
/// The shapes here are final; the implementation currently uses
/// placeholder bytes so the build has no hard dependency on
/// OpenSSL. A follow-up commit promotes msquic's bundled
/// @c openssl3 target into a shared dependency and swaps the
/// impl for real Ed25519 sign/verify + AES-256-GCM AEAD.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace xorio::orchestrator::crypto {

/// @brief 32-byte Ed25519 public key (the @c PcId on the wire).
struct PairingPublicKey {
    std::array<std::uint8_t, 32> bytes{};

    /// @brief Lowercase hex fingerprint used as the @c machine_id.
    std::string fingerprint() const;

    friend bool operator==(const PairingPublicKey& a,
                           const PairingPublicKey& b) {
        return a.bytes == b.bytes;
    }
};

/// @brief 64-byte Ed25519 secret key (never leaves the owning process).
struct PairingSecretKey {
    std::array<std::uint8_t, 64> bytes{};
};

/// @brief Ed25519 signature over an arbitrary byte string.
struct Signature {
    std::array<std::uint8_t, 64> bytes{};
};

/// @brief Bundled key-pair produced by @ref generate_pairing_key.
struct PairingKey {
    PairingPublicKey public_key;
    PairingSecretKey secret_key;
};

/// @brief Generate a fresh Ed25519 key pair.
/// @note Placeholder implementation — returns deterministic bytes
/// seeded from a process-unique counter. Real entropy wires in
/// with the OpenSSL integration commit.
PairingKey generate_pairing_key();

/// @brief Sign @p message with @p sk.
Signature sign(const PairingSecretKey& sk,
               const std::uint8_t* message, std::size_t len);

/// @brief Verify that @p sig was produced by @p pk over @p message.
bool verify(const PairingPublicKey& pk,
            const std::uint8_t* message, std::size_t len,
            const Signature& sig);

/// @brief Typed container for a CRDT record plus its signature.
/// @tparam T Payload type (e.g. @c CapsRecord, @c LayoutRecord).
template <typename T>
struct SignedRecord {
    T                payload;
    std::uint64_t    version = 0;   ///< LWW timestamp (ns since epoch).
    PairingPublicKey writer;        ///< Origin peer.
    Signature        signature;     ///< Over (version || writer || payload_bytes).
};

/// @brief Opaque symmetric key derived from the OS keyring (on
/// real builds) or a machine-derived KDF (placeholder).
struct SymmetricKey {
    std::array<std::uint8_t, 32> bytes{};
};

/// @brief AEAD encrypt @p plaintext.
/// @return Ciphertext preceded by a 12-byte nonce and followed by
/// a 16-byte authentication tag.
std::vector<std::uint8_t>
aead_encrypt(const SymmetricKey& key,
             const std::uint8_t* plaintext, std::size_t len);

/// @brief AEAD decrypt @p ciphertext produced by @ref aead_encrypt.
/// @return Empty vector on authentication failure.
std::vector<std::uint8_t>
aead_decrypt(const SymmetricKey& key,
             const std::uint8_t* ciphertext, std::size_t len);

/// @brief Derive a local-machine symmetric key from OS attributes.
/// @note Placeholder implementation hashes @c uid + @c hostname.
/// Real implementation reads from an OS keyring entry created on
/// first launch.
SymmetricKey derive_machine_key();

}  // namespace xorio::orchestrator::crypto
