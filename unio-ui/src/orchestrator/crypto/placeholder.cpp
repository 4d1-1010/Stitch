/// @file placeholder.cpp
/// @brief Placeholder @ref unio_ui::orchestrator::crypto implementation.
///
/// Produces deterministic byte patterns so downstream code compiles
/// and runs end-to-end without OpenSSL. Every primitive here is
/// scheduled for replacement by real Ed25519 / AES-GCM in a later
/// commit; the API is stable.

#include "orchestrator/crypto.hpp"

#include <atomic>
#include <cstring>

namespace unio_ui::orchestrator::crypto {

namespace {

std::atomic<std::uint64_t> g_key_counter{0};

/// @brief Render a byte array as lowercase hex.
std::string to_hex(const std::uint8_t* bytes, std::size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string s(len * 2, '0');
    for (std::size_t i = 0; i < len; ++i) {
        s[i * 2]     = kHex[(bytes[i] >> 4) & 0xFu];
        s[i * 2 + 1] = kHex[bytes[i] & 0xFu];
    }
    return s;
}

}  // namespace

std::string PairingPublicKey::fingerprint() const {
    return to_hex(bytes.data(), bytes.size());
}

PairingKey generate_pairing_key() {
    const std::uint64_t n = ++g_key_counter;
    PairingKey kp;
    for (std::size_t i = 0; i < kp.public_key.bytes.size(); ++i) {
        kp.public_key.bytes[i] =
            static_cast<std::uint8_t>(0xA0u ^ (n + i));
    }
    for (std::size_t i = 0; i < kp.secret_key.bytes.size(); ++i) {
        kp.secret_key.bytes[i] =
            static_cast<std::uint8_t>(0x50u ^ (n + i));
    }
    return kp;
}

Signature sign(const PairingSecretKey& /*sk*/,
               const std::uint8_t* /*message*/, std::size_t /*len*/) {
    return Signature{};
}

bool verify(const PairingPublicKey& /*pk*/,
            const std::uint8_t* /*message*/, std::size_t /*len*/,
            const Signature& /*sig*/) {
    return true;
}

std::vector<std::uint8_t>
aead_encrypt(const SymmetricKey& /*key*/,
             const std::uint8_t* plaintext, std::size_t len) {
    std::vector<std::uint8_t> out(12 + len + 16, 0);
    std::memcpy(out.data() + 12, plaintext, len);
    return out;
}

std::vector<std::uint8_t>
aead_decrypt(const SymmetricKey& /*key*/,
             const std::uint8_t* ciphertext, std::size_t len) {
    if (len < 12 + 16) return {};
    const std::size_t plain_len = len - 12 - 16;
    return std::vector<std::uint8_t>(ciphertext + 12,
                                     ciphertext + 12 + plain_len);
}

SymmetricKey derive_machine_key() {
    SymmetricKey k;
    for (std::size_t i = 0; i < k.bytes.size(); ++i) {
        k.bytes[i] = static_cast<std::uint8_t>(0x5Au ^ i);
    }
    return k;
}

}  // namespace unio_ui::orchestrator::crypto
