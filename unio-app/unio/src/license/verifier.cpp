/// @file verifier.cpp
/// @brief Ed25519 verification of pasted licence tokens against
/// the embedded vendor public key.

#include "license/verifier.hpp"

#include "license/codec.hpp"
#include "license/vendor_key.hpp"

#include <openssl/evp.h>

#include <utility>

namespace unio_ui::license {

VerifyResult verify_wrapped_token(const std::string& wrapped) {
    VerifyResult r;

    auto unwrapped = codec::decode_wrapped(wrapped);
    if (!unwrapped) {
        r.error = VerifyError::BadFormat;
        return r;
    }
    auto& [payload, signature] = *unwrapped;

    auto token = codec::decode_payload(payload.data(), payload.size());
    if (!token) {
        r.error = VerifyError::BadPayload;
        return r;
    }

    if (token->issuer_key_id != kVendorKeyId) {
        r.error = VerifyError::UnknownIssuer;
        return r;
    }

    const auto& pubkey_bytes = vendor_public_key();
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr,
        pubkey_bytes.data(), pubkey_bytes.size());
    if (!pkey) {
        r.error = VerifyError::BadSignature;
        return r;
    }

    bool ok = false;
    if (EVP_MD_CTX* ctx = EVP_MD_CTX_new(); ctx) {
        if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey) == 1) {
            ok = EVP_DigestVerify(ctx,
                                  signature.data(), signature.size(),
                                  payload.data(), payload.size()) == 1;
        }
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);

    if (!ok) {
        r.error = VerifyError::BadSignature;
        return r;
    }

    r.error                  = VerifyError::Ok;
    r.signed_token.token     = std::move(*token);
    r.signed_token.signature = signature;
    return r;
}

}  // namespace unio_ui::license
