/// @file machine_key.cpp
/// @brief Per-machine symmetric key derivation —
/// @ref derive_machine_key from @c orchestrator/crypto.hpp.
///
/// Gathers host-stable identifiers (uid + hostname + machine-id
/// on Linux; user + computer name on Windows) and runs them
/// through HKDF-SHA256 to produce a deterministic 32-byte key
/// bound to the current account-on-machine. The key changes if
/// the user, hostname, or machine identity changes; it survives
/// reboots and binary upgrades.
///
/// **Bridge implementation.** The shipped path stores symmetric
/// material in an OS keyring (Secret Service / DPAPI) — that
/// lands in the secure-store commit. Until then this derivation
/// gives the AEAD callers a stable real key so the rest of the
/// orchestrator can be exercised end-to-end.
///
/// Backed by OpenSSL libcrypto (`EVP_PKEY_HKDF`). Linked via the
/// `unio::crypto` interface target — see
/// `unio-ui/cmake/UnioCrypto.cmake`.

#include "orchestrator/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/kdf.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace unio_ui::orchestrator::crypto {

namespace {

/// @brief HKDF "info" string — domain-separates this derivation
/// from any future use of the same input material.
constexpr char kInfo[] = "unio-machine-key";

/// @brief Append a NUL-terminated C-string (no terminator) to @p out.
void append_str(std::vector<std::uint8_t>& out, const char* s) {
    if (!s) return;
    while (*s) out.push_back(static_cast<std::uint8_t>(*s++));
}

/// @brief Gather host-stable input keying material.
std::vector<std::uint8_t> gather_ikm() {
    std::vector<std::uint8_t> ikm;
    ikm.reserve(256);

#if defined(_WIN32)
    append_str(ikm, std::getenv("USERNAME"));
    append_str(ikm, std::getenv("COMPUTERNAME"));
    // Future hardening: pull HKLM\SOFTWARE\Microsoft\Cryptography
    // MachineGuid for stronger machine binding. Acceptable for
    // bridge use today — the secure-store commit replaces the
    // whole derivation with a keyring lookup anyway.
#else
    const uid_t uid = ::getuid();
    ikm.insert(ikm.end(),
               reinterpret_cast<const std::uint8_t*>(&uid),
               reinterpret_cast<const std::uint8_t*>(&uid) + sizeof(uid));

    char host[256] = {};
    if (::gethostname(host, sizeof(host) - 1) == 0) {
        append_str(ikm, host);
    }

    std::ifstream f("/etc/machine-id");
    std::string mid;
    std::getline(f, mid);
    ikm.insert(ikm.end(), mid.begin(), mid.end());
#endif

    return ikm;
}

}  // namespace

SymmetricKey derive_machine_key() {
    SymmetricKey out{};

    const std::vector<std::uint8_t> ikm = gather_ikm();

    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!pctx) return out;

    std::size_t out_len = out.bytes.size();
    const bool  ok =
        EVP_PKEY_derive_init(pctx) == 1
        && EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) == 1
        && EVP_PKEY_CTX_set1_hkdf_key(
               pctx, ikm.data(),
               static_cast<int>(ikm.size())) == 1
        && EVP_PKEY_CTX_add1_hkdf_info(
               pctx,
               reinterpret_cast<const unsigned char*>(kInfo),
               static_cast<int>(sizeof(kInfo) - 1)) == 1
        && EVP_PKEY_derive(pctx, out.bytes.data(), &out_len) == 1
        && out_len == out.bytes.size();

    EVP_PKEY_CTX_free(pctx);
    return ok ? out : SymmetricKey{};
}

}  // namespace unio_ui::orchestrator::crypto
