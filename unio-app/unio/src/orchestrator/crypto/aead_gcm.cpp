/// @file aead_gcm.cpp
/// @brief AES-256-GCM authenticated encryption — the symmetric
/// half of @c orchestrator/crypto.hpp.
///
/// Wire format produced by @ref aead_encrypt and consumed by
/// @ref aead_decrypt:
///
///   [ nonce(12) ][ ciphertext(N) ][ tag(16) ]
///
/// The nonce is fresh per-call from `RAND_bytes`. The 12 + 16
/// overhead is constant; callers size their buffers accordingly.
///
/// Backed by OpenSSL libcrypto (`EVP_aes_256_gcm`). Linked via
/// the `unio::crypto` interface target — see
/// `unio-ui/cmake/UnioCrypto.cmake`.

#include "orchestrator/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace unio_ui::orchestrator::crypto {

namespace {

constexpr int kNonceLen = 12;
constexpr int kTagLen   = 16;

}  // namespace

std::vector<std::uint8_t>
aead_encrypt(const SymmetricKey& key,
             const std::uint8_t* plaintext, std::size_t len) {
    std::vector<std::uint8_t> out(kNonceLen + len + kTagLen);

    if (RAND_bytes(out.data(), kNonceLen) != 1) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    bool ok     = true;
    int  outlen = 0;
    int  finlen = 0;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                           key.bytes.data(), out.data()) != 1) ok = false;
    if (ok && EVP_EncryptUpdate(ctx, out.data() + kNonceLen, &outlen,
                                 plaintext,
                                 static_cast<int>(len)) != 1) ok = false;
    if (ok && EVP_EncryptFinal_ex(ctx,
                                   out.data() + kNonceLen + outlen,
                                   &finlen) != 1) ok = false;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen,
                                   out.data() + kNonceLen + len) != 1) ok = false;

    EVP_CIPHER_CTX_free(ctx);
    return ok ? out : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t>
aead_decrypt(const SymmetricKey& key,
             const std::uint8_t* ciphertext, std::size_t len) {
    if (len < static_cast<std::size_t>(kNonceLen + kTagLen)) return {};
    const std::size_t plain_len = len - kNonceLen - kTagLen;

    std::vector<std::uint8_t> out(plain_len);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    bool ok     = true;
    int  outlen = 0;
    int  finlen = 0;

    // OpenSSL's ctrl() interface takes a non-const void* even
    // though @c GCM_SET_TAG only reads. Cast away const; safe on
    // every supported libcrypto version.
    auto* tag_bytes =
        const_cast<std::uint8_t*>(ciphertext + kNonceLen + plain_len);

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
                           key.bytes.data(), ciphertext) != 1) ok = false;
    if (ok && EVP_DecryptUpdate(ctx, out.data(), &outlen,
                                 ciphertext + kNonceLen,
                                 static_cast<int>(plain_len)) != 1) ok = false;
    if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen,
                                   tag_bytes) != 1) ok = false;
    if (ok && EVP_DecryptFinal_ex(ctx,
                                   out.data() + outlen,
                                   &finlen) != 1) ok = false;

    EVP_CIPHER_CTX_free(ctx);
    return ok ? out : std::vector<std::uint8_t>{};
}

}  // namespace unio_ui::orchestrator::crypto
