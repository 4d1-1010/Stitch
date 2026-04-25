/// @file ed25519.cpp
/// @brief Ed25519 keypair generation, signing, and verification —
/// the asymmetric half of @c orchestrator/crypto.hpp.
///
/// Backed by OpenSSL libcrypto (`EVP_PKEY_ED25519`). Linked via
/// the `unio::crypto` interface target — see
/// `unio-ui/cmake/UnioCrypto.cmake`.

#include "orchestrator/crypto.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstring>
#include <string>

namespace unio_ui::orchestrator::crypto {

namespace {

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
    PairingKey kp;

    EVP_PKEY*     pkey = nullptr;
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!pctx
        || EVP_PKEY_keygen_init(pctx) != 1
        || EVP_PKEY_keygen(pctx, &pkey) != 1) {
        EVP_PKEY_CTX_free(pctx);
        EVP_PKEY_free(pkey);
        return kp;  // Zero-filled; caller treats as failure.
    }
    EVP_PKEY_CTX_free(pctx);

    std::size_t pub_len = kp.public_key.bytes.size();
    if (EVP_PKEY_get_raw_public_key(pkey,
                                    kp.public_key.bytes.data(),
                                    &pub_len) != 1
        || pub_len != kp.public_key.bytes.size()) {
        EVP_PKEY_free(pkey);
        return PairingKey{};
    }

    std::array<std::uint8_t, 32> priv{};
    std::size_t                  priv_len = priv.size();
    if (EVP_PKEY_get_raw_private_key(pkey, priv.data(), &priv_len) != 1
        || priv_len != priv.size()) {
        EVP_PKEY_free(pkey);
        return PairingKey{};
    }
    EVP_PKEY_free(pkey);

    // Pack secret bytes as priv(32) || pub(32) — libsodium-style.
    // Sign uses bytes[0..32]; the trailing public half lets a
    // caller holding only the secret derive the public side
    // without a second OpenSSL round-trip.
    std::memcpy(kp.secret_key.bytes.data(),       priv.data(),                32);
    std::memcpy(kp.secret_key.bytes.data() + 32,  kp.public_key.bytes.data(), 32);
    return kp;
}

Signature sign(const PairingSecretKey& sk,
               const std::uint8_t* message, std::size_t len) {
    Signature out{};

    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, sk.bytes.data(), 32);
    if (!pkey) return out;

    if (EVP_MD_CTX* ctx = EVP_MD_CTX_new(); ctx) {
        if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
            std::size_t sig_len = out.bytes.size();
            if (EVP_DigestSign(ctx, out.bytes.data(), &sig_len,
                               message, len) != 1
                || sig_len != out.bytes.size()) {
                out = Signature{};
            }
        }
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    return out;
}

bool verify(const PairingPublicKey& pk,
            const std::uint8_t* message, std::size_t len,
            const Signature& sig) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, pk.bytes.data(), pk.bytes.size());
    if (!pkey) return false;

    bool ok = false;
    if (EVP_MD_CTX* ctx = EVP_MD_CTX_new(); ctx) {
        if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
            ok = EVP_DigestVerify(ctx, sig.bytes.data(), sig.bytes.size(),
                                  message, len) == 1;
        }
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    return ok;
}

}  // namespace unio_ui::orchestrator::crypto
