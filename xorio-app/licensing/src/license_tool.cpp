/// @file main.cpp
/// @brief `xorio-license-tool` — CLI that signs a licence token
/// with the vendor Ed25519 private key and prints it to stdout.

#include "license/codec.hpp"
#include "license/token.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

using xorio::license::LicenseToken;
using xorio::license::Tier;

/// @brief Map a tier keyword to the enum value. Returns -1 on
/// unknown input.
int parse_tier(const std::string& s) {
    if (s == "trial")      return static_cast<int>(Tier::Trial);
    if (s == "home")       return static_cast<int>(Tier::Home);
    if (s == "pro")        return static_cast<int>(Tier::Pro);
    if (s == "team")       return static_cast<int>(Tier::Team);
    if (s == "enterprise") return static_cast<int>(Tier::Enterprise);
    return -1;
}

/// @brief Parse a duration spec like "5d", "24h", "3600s" into
/// a total seconds count. Returns 0 on failure.
std::uint64_t parse_duration(const std::string& s) {
    if (s.empty()) return 0;
    std::uint64_t n = 0;
    std::size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        n = n * 10 + static_cast<std::uint64_t>(s[i] - '0');
    }
    if (i == 0 || i == s.size()) return i == s.size() ? n : 0;
    const char unit = s[i];
    if (i + 1 != s.size()) return 0;
    switch (unit) {
        case 's': return n;
        case 'm': return n * 60;
        case 'h': return n * 3600;
        case 'd': return n * 86400;
        default:  return 0;
    }
}

/// @brief Read a PEM-encoded Ed25519 private key. Dies on failure.
EVP_PKEY* load_private_key(const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp) {
        std::fprintf(stderr, "cannot open %s: %s\n",
                     path.c_str(), std::strerror(errno));
        std::exit(1);
    }
    EVP_PKEY* key = PEM_read_PrivateKey(fp, nullptr, nullptr, nullptr);
    std::fclose(fp);
    if (!key) {
        std::fprintf(stderr, "failed to parse Ed25519 PEM at %s\n",
                     path.c_str());
        std::exit(1);
    }
    return key;
}

/// @brief Ed25519 sign @p message with @p key. Dies on failure.
std::array<std::uint8_t, 64>
ed25519_sign(EVP_PKEY* key,
             const std::uint8_t* message, std::size_t len) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) != 1) {
        std::fprintf(stderr, "EVP_DigestSignInit failed\n");
        std::exit(1);
    }
    std::size_t sig_len = 64;
    std::array<std::uint8_t, 64> sig{};
    if (EVP_DigestSign(ctx, sig.data(), &sig_len, message, len) != 1
        || sig_len != 64) {
        std::fprintf(stderr, "EVP_DigestSign failed\n");
        std::exit(1);
    }
    EVP_MD_CTX_free(ctx);
    return sig;
}

/// @brief Generate 16 random bytes for the licence-id UUID.
std::array<std::uint8_t, 16> random_uuid() {
    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::array<std::uint8_t, 16> id{};
    for (std::size_t i = 0; i < 16; i += 8) {
        const std::uint64_t w = rng();
        for (std::size_t j = 0; j < 8; ++j) {
            id[i + j] = static_cast<std::uint8_t>(w >> (8 * j));
        }
    }
    id[6] = static_cast<std::uint8_t>((id[6] & 0x0Fu) | 0x40u);  // v4
    id[8] = static_cast<std::uint8_t>((id[8] & 0x3Fu) | 0x80u);  // variant
    return id;
}

/// @brief Print usage and exit.
[[noreturn]] void usage() {
    std::fprintf(stderr,
        "xorio-license-tool — sign a licence token\n\n"
        "Usage:\n"
        "  xorio-license-tool --key <private.pem> --customer <id>\n"
        "                    --tier <trial|home|pro|team|enterprise>\n"
        "                    --duration <Ns|Nm|Nh|Nd>\n"
        "                    [--valid-from-epoch <seconds>]\n"
        "                    [--issuer-key-id <u32>]\n");
    std::exit(2);
}

/// @brief Return the value for a long-form flag, or die if absent.
std::string take_arg(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    std::fprintf(stderr, "missing flag: %s\n", flag.c_str());
    usage();
}

/// @brief Return the value for an optional flag; @c fallback if absent.
std::string take_arg_or(int argc, char** argv,
                        const std::string& flag,
                        const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == flag) return argv[i + 1];
    }
    return fallback;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) usage();

    const std::string key_path   = take_arg(argc, argv, "--key");
    const std::string customer   = take_arg(argc, argv, "--customer");
    const std::string tier_name  = take_arg(argc, argv, "--tier");
    const std::string dur_spec   = take_arg(argc, argv, "--duration");
    const std::string vf_epoch   = take_arg_or(argc, argv, "--valid-from-epoch", "");
    const std::string issuer_id  = take_arg_or(argc, argv, "--issuer-key-id", "0");

    const int tier = parse_tier(tier_name);
    if (tier < 0) {
        std::fprintf(stderr, "unknown tier: %s\n", tier_name.c_str());
        return 2;
    }
    const std::uint64_t duration = parse_duration(dur_spec);
    if (duration == 0) {
        std::fprintf(stderr, "invalid duration: %s\n", dur_spec.c_str());
        return 2;
    }

    const std::uint64_t valid_from_s = vf_epoch.empty()
        ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch()).count())
        : static_cast<std::uint64_t>(std::stoull(vf_epoch));

    LicenseToken t;
    t.schema_version   = 1;
    t.license_id       = random_uuid();
    t.customer         = customer;
    t.tier             = static_cast<Tier>(tier);
    t.issued_at_ns     = valid_from_s * 1'000'000'000ull;
    t.valid_from_ns    = t.issued_at_ns;
    t.duration_seconds = duration;
    t.expires_at_ns    = t.valid_from_ns + duration * 1'000'000'000ull;
    t.issuer_key_id    = static_cast<std::uint32_t>(std::stoul(issuer_id));

    const std::vector<std::uint8_t> payload =
        xorio::license::codec::encode_payload(t);

    EVP_PKEY* key = load_private_key(key_path);
    const std::array<std::uint8_t, 64> signature =
        ed25519_sign(key, payload.data(), payload.size());
    EVP_PKEY_free(key);

    const std::string wrapped =
        xorio::license::codec::encode_wrapped(payload, signature);
    std::puts(wrapped.c_str());
    return 0;
}
