/// @file codec.cpp
/// @brief Binary + base64 serialisation for licence tokens.

#include "license/codec.hpp"

#include <cstring>
#include <string>

namespace xorio::license::codec {

namespace {

/// @brief Append @p value to @p out in little-endian byte order.
template <typename T>
void append_le(std::vector<std::uint8_t>& out, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

/// @brief Read a little-endian integer of type @p T from @p p.
template <typename T>
T read_le(const std::uint8_t* p) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(p[i]) << (8 * i);
    }
    return value;
}

/// @brief Standard base64 alphabet, no padding folding.
constexpr const char* kB64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// @brief Reverse lookup for @c kB64; -1 marks invalid chars.
std::int8_t b64_lookup(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<std::int8_t>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<std::int8_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<std::int8_t>(c - '0' + 52);
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::vector<std::uint8_t> encode_payload(const LicenseToken& t) {
    std::vector<std::uint8_t> out;
    out.reserve(128);
    append_le<std::uint32_t>(out, kPayloadMagic);
    append_le<std::uint32_t>(out, t.schema_version);
    out.insert(out.end(), t.license_id.begin(), t.license_id.end());
    append_le<std::uint32_t>(out, static_cast<std::uint32_t>(t.customer.size()));
    out.insert(out.end(), t.customer.begin(), t.customer.end());
    append_le<std::uint32_t>(out, static_cast<std::uint32_t>(t.tier));
    append_le<std::uint64_t>(out, t.issued_at_ns);
    append_le<std::uint64_t>(out, t.valid_from_ns);
    append_le<std::uint64_t>(out, t.duration_seconds);
    append_le<std::uint64_t>(out, t.expires_at_ns);
    append_le<std::uint32_t>(out, t.issuer_key_id);
    return out;
}

std::optional<LicenseToken>
decode_payload(const std::uint8_t* bytes, std::size_t len) {
    std::size_t o = 0;
    auto need = [&](std::size_t n) { return o + n <= len; };

    if (!need(4) || read_le<std::uint32_t>(bytes + o) != kPayloadMagic) {
        return std::nullopt;
    }
    o += 4;

    LicenseToken t;
    if (!need(4)) return std::nullopt;
    t.schema_version = read_le<std::uint32_t>(bytes + o); o += 4;

    if (!need(16)) return std::nullopt;
    std::memcpy(t.license_id.data(), bytes + o, 16); o += 16;

    if (!need(4)) return std::nullopt;
    const auto c_len = read_le<std::uint32_t>(bytes + o); o += 4;
    if (!need(c_len)) return std::nullopt;
    t.customer.assign(reinterpret_cast<const char*>(bytes + o), c_len);
    o += c_len;

    if (!need(4)) return std::nullopt;
    t.tier = static_cast<Tier>(read_le<std::uint32_t>(bytes + o)); o += 4;

    if (!need(8 + 8 + 8 + 8 + 4)) return std::nullopt;
    t.issued_at_ns     = read_le<std::uint64_t>(bytes + o); o += 8;
    t.valid_from_ns    = read_le<std::uint64_t>(bytes + o); o += 8;
    t.duration_seconds = read_le<std::uint64_t>(bytes + o); o += 8;
    t.expires_at_ns    = read_le<std::uint64_t>(bytes + o); o += 8;
    t.issuer_key_id    = read_le<std::uint32_t>(bytes + o); o += 4;

    if (o != len) return std::nullopt;
    return t;
}

std::string
encode_wrapped(const std::vector<std::uint8_t>& payload,
               const std::array<std::uint8_t, 64>& signature) {
    std::vector<std::uint8_t> full;
    full.reserve(payload.size() + signature.size());
    full.insert(full.end(), payload.begin(), payload.end());
    full.insert(full.end(), signature.begin(), signature.end());
    std::string out = kWrapPrefix;
    out += b64_encode(full.data(), full.size());
    return out;
}

std::optional<std::pair<std::vector<std::uint8_t>,
                        std::array<std::uint8_t, 64>>>
decode_wrapped(const std::string& wrapped) {
    const std::string prefix = kWrapPrefix;
    if (wrapped.size() < prefix.size() ||
        wrapped.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    auto decoded = b64_decode(wrapped.substr(prefix.size()));
    if (!decoded || decoded->size() < 64) return std::nullopt;
    std::array<std::uint8_t, 64> sig{};
    const std::size_t payload_len = decoded->size() - 64;
    std::memcpy(sig.data(), decoded->data() + payload_len, 64);
    std::vector<std::uint8_t> payload(decoded->begin(),
                                      decoded->begin() + payload_len);
    return std::make_pair(std::move(payload), sig);
}

std::string b64_encode(const std::uint8_t* bytes, std::size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                     (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                      static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(kB64[(triple >> 18) & 0x3Fu]);
        out.push_back(kB64[(triple >> 12) & 0x3Fu]);
        out.push_back(kB64[(triple >>  6) & 0x3Fu]);
        out.push_back(kB64[ triple        & 0x3Fu]);
    }
    if (i < len) {
        std::uint32_t triple = static_cast<std::uint32_t>(bytes[i]) << 16;
        if (i + 1 < len) triple |= static_cast<std::uint32_t>(bytes[i + 1]) << 8;
        out.push_back(kB64[(triple >> 18) & 0x3Fu]);
        out.push_back(kB64[(triple >> 12) & 0x3Fu]);
        if (i + 1 < len) out.push_back(kB64[(triple >> 6) & 0x3Fu]);
        else             out.push_back('=');
        out.push_back('=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>>
b64_decode(const std::string& input) {
    std::vector<std::uint8_t> out;
    out.reserve((input.size() / 4) * 3);
    std::uint32_t buf = 0;
    int bits = 0;
    for (char c : input) {
        if (c == '=') break;
        const std::int8_t v = b64_lookup(c);
        if (v < 0) return std::nullopt;
        buf = (buf << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFFu));
        }
    }
    return out;
}

}  // namespace xorio::license::codec
