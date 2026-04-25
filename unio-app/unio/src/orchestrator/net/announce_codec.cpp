/// @file announce_codec.cpp
/// @brief Hand-rolled JSON encode/decode for the discovery
/// announce schema. Pure functions, no I/O, no global state.
///
/// The implementation deliberately stops short of being a general
/// JSON parser — it accepts only the fixed shape declared in
/// `announce_codec.hpp` and rejects anything else. Keeping the
/// scope narrow lets us own this small TU end-to-end without
/// pulling in a JSON dependency.

#include "orchestrator/net/announce_codec.hpp"

#include <cctype>
#include <charconv>
#include <cstring>
#include <string>
#include <string_view>

namespace unio_ui::orchestrator::net {

namespace {

// ── Encode helpers ─────────────────────────────────────────────

void append_str(std::vector<std::uint8_t>& out, std::string_view s) {
    out.insert(out.end(), s.begin(), s.end());
}

void append_json_string(std::vector<std::uint8_t>& out,
                        std::string_view value) {
    out.push_back('"');
    for (char c : value) {
        // The two characters that MUST be escaped to keep a
        // syntactically-valid JSON string. The full RFC requires
        // also escaping U+0000..U+001F; in practice machine-id and
        // hostname are printable-ASCII / UTF-8 sequences, so we
        // limit the table to the two characters that can occur.
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<std::uint8_t>(c));
        } else {
            out.push_back(static_cast<std::uint8_t>(c));
        }
    }
    out.push_back('"');
}

/// @brief Pack a single display into the canonical CSV form
/// `name:x,y,w,h`. Only the three CSV delimiters (`|:,`) and the
/// JSON-string-killer `"` need escaping; backslashes survive
/// untouched because the outer JSON-string encoder will escape
/// them itself.
std::string sanitise_id(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '|' || c == ':' || c == ',' || c == '"') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// @brief Encode the displays vector as `name:x,y,w,h|name:x,y,w,h|…`.
/// One JSON string field; cheap to fit into a single UDP datagram.
std::string encode_displays_csv(const std::vector<AnnounceDisplay>& ds) {
    std::string out;
    for (std::size_t i = 0; i < ds.size(); ++i) {
        if (i != 0) out.push_back('|');
        out += sanitise_id(ds[i].monitor_id);
        out.push_back(':');
        out += std::to_string(ds[i].global_x);
        out.push_back(',');
        out += std::to_string(ds[i].global_y);
        out.push_back(',');
        out += std::to_string(ds[i].width);
        out.push_back(',');
        out += std::to_string(ds[i].height);
    }
    return out;
}

/// @brief Inverse of @ref encode_displays_csv. Skips malformed
/// entries silently — the receiver shows whatever fields parsed
/// cleanly.
std::vector<AnnounceDisplay> decode_displays_csv(std::string_view s) {
    std::vector<AnnounceDisplay> out;

    auto next_token = [](std::string_view in, char sep,
                         std::size_t& pos) -> std::string_view {
        const std::size_t start = pos;
        while (pos < in.size() && in[pos] != sep) ++pos;
        std::string_view t = in.substr(start, pos - start);
        if (pos < in.size()) ++pos;       // consume the separator
        return t;
    };

    std::size_t i = 0;
    while (i < s.size()) {
        // Carve out one entry up to the next `|`.
        std::size_t entry_pos = 0;
        std::string_view entry = next_token(s, '|', i);
        AnnounceDisplay d;

        const std::string_view name = next_token(entry, ':', entry_pos);
        if (name.empty() || entry_pos >= entry.size()) continue;
        d.monitor_id.assign(name.data(), name.size());

        std::int32_t* fields[4] = {
            &d.global_x, &d.global_y, &d.width, &d.height
        };
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            std::string_view v = next_token(entry, ',', entry_pos);
            if (v.empty()) { ok = false; break; }
            const auto r = std::from_chars(v.data(), v.data() + v.size(),
                                           *fields[k]);
            if (r.ec != std::errc{}) { ok = false; break; }
        }
        if (ok && d.width > 0 && d.height > 0) out.push_back(std::move(d));
    }
    return out;
}

// ── Decode helpers ─────────────────────────────────────────────
//
// The parser is single-pass + one-shot: it walks through the input
// from start to end, advancing `pos` past consumed bytes. Any
// failure short-circuits to nullopt by setting `pos` past `end`.

void skip_ws(const std::uint8_t* data, std::size_t& pos, std::size_t end) {
    while (pos < end) {
        const char c = static_cast<char>(data[pos]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
        else break;
    }
}

bool consume_char(const std::uint8_t* data, std::size_t& pos,
                  std::size_t end, char expected) {
    skip_ws(data, pos, end);
    if (pos >= end || static_cast<char>(data[pos]) != expected) return false;
    ++pos;
    return true;
}

/// @brief Parse a JSON string literal starting at `pos`.
/// On success, advances `pos` past the closing quote and returns
/// the unescaped contents. On failure, leaves `pos` undefined and
/// returns `nullopt`.
std::optional<std::string>
parse_string(const std::uint8_t* data, std::size_t& pos, std::size_t end) {
    skip_ws(data, pos, end);
    if (pos >= end || data[pos] != '"') return std::nullopt;
    ++pos;
    std::string out;
    out.reserve(64);
    while (pos < end) {
        const std::uint8_t c = data[pos];
        if (c == '"') { ++pos; return out; }
        if (c == '\\') {
            // Mirror the encoder's restricted alphabet plus a
            // permissive read for forward-compat: decode a few
            // common JSON escapes; any other escape is treated as
            // literal pass-through to keep noise from a future
            // sender's extension out of the failure path.
            ++pos;
            if (pos >= end) return std::nullopt;
            const char esc = static_cast<char>(data[pos]);
            switch (esc) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                default:   out.push_back(esc);  break;
            }
            ++pos;
        } else {
            out.push_back(static_cast<char>(c));
            ++pos;
        }
    }
    return std::nullopt;  // unterminated string.
}

/// @brief Parse a JSON integer literal into a 64-bit unsigned
/// value. Negative numbers are rejected — every numeric field in
/// the schema is a non-negative quantity.
std::optional<std::uint64_t>
parse_uint(const std::uint8_t* data, std::size_t& pos, std::size_t end) {
    skip_ws(data, pos, end);
    const std::size_t start = pos;
    while (pos < end && std::isdigit(data[pos])) ++pos;
    if (pos == start) return std::nullopt;

    std::uint64_t v = 0;
    const auto* first = reinterpret_cast<const char*>(data + start);
    const auto* last  = reinterpret_cast<const char*>(data + pos);
    const auto r = std::from_chars(first, last, v);
    if (r.ec != std::errc{} || r.ptr != last) return std::nullopt;
    return v;
}

/// @brief Parse a JSON `true` / `false` literal.
std::optional<bool>
parse_bool(const std::uint8_t* data, std::size_t& pos, std::size_t end) {
    skip_ws(data, pos, end);
    if (pos + 4 <= end
        && std::memcmp(data + pos, "true", 4) == 0) {
        pos += 4;
        return true;
    }
    if (pos + 5 <= end
        && std::memcmp(data + pos, "false", 5) == 0) {
        pos += 5;
        return false;
    }
    return std::nullopt;
}

/// @brief Skip a JSON value of any type starting at `pos`. Used to
/// tolerate unknown fields a future sender might add — we walk
/// past them without trying to interpret. Returns `false` if the
/// value is malformed.
bool skip_value(const std::uint8_t* data, std::size_t& pos, std::size_t end) {
    skip_ws(data, pos, end);
    if (pos >= end) return false;
    const char c = static_cast<char>(data[pos]);
    if (c == '"') {
        return parse_string(data, pos, end).has_value();
    }
    if (c == 't' || c == 'f') {
        return parse_bool(data, pos, end).has_value();
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
        if (c == '-') ++pos;
        return parse_uint(data, pos, end).has_value();
    }
    if (c == 'n' && pos + 4 <= end
        && std::memcmp(data + pos, "null", 4) == 0) {
        pos += 4;
        return true;
    }
    // Object / array skipping isn't needed by this schema; reject
    // so we don't silently accept malformed nested structures.
    return false;
}

}  // namespace

std::vector<std::uint8_t> encode_announce(const AnnouncePayload& p) {
    std::vector<std::uint8_t> out;
    out.reserve(160);

    append_str(out, R"({"magic":")");
    append_str(out, kAnnounceMagic);
    append_str(out, R"(","machine_id":)");
    append_json_string(out, p.machine_id);
    append_str(out, R"(,"hostname":)");
    append_json_string(out, p.hostname);
    append_str(out, R"(,"tcp_port":)");
    char    buf[8];
    const auto r = std::to_chars(buf, buf + sizeof(buf), p.tcp_port);
    out.insert(out.end(),
               reinterpret_cast<const std::uint8_t*>(buf),
               reinterpret_cast<const std::uint8_t*>(r.ptr));
    append_str(out, R"(,"authed":)");
    append_str(out, p.authed ? "true" : "false");

    // Optional `displays_csv` extension. Omitted entirely when
    // empty so wire-format-compatible Python decoders that don't
    // know the field don't see an unexpected key — the C++
    // decoder accepts both shapes.
    if (!p.displays.empty()) {
        append_str(out, R"(,"displays_csv":)");
        append_json_string(out, encode_displays_csv(p.displays));
    }
    out.push_back('}');
    return out;
}

std::optional<AnnouncePayload>
decode_announce(const std::uint8_t* bytes, std::size_t len) {
    std::size_t pos = 0;
    if (!consume_char(bytes, pos, len, '{')) return std::nullopt;

    AnnouncePayload out;
    bool saw_magic      = false;
    bool saw_machine_id = false;
    bool saw_hostname   = false;
    bool saw_tcp_port   = false;

    while (true) {
        skip_ws(bytes, pos, len);
        if (pos < len && bytes[pos] == '}') { ++pos; break; }

        auto key = parse_string(bytes, pos, len);
        if (!key) return std::nullopt;
        if (!consume_char(bytes, pos, len, ':')) return std::nullopt;

        if (*key == "magic") {
            auto v = parse_string(bytes, pos, len);
            if (!v || *v != kAnnounceMagic) return std::nullopt;
            saw_magic = true;
        } else if (*key == "machine_id") {
            auto v = parse_string(bytes, pos, len);
            if (!v) return std::nullopt;
            out.machine_id = std::move(*v);
            saw_machine_id = true;
        } else if (*key == "hostname") {
            auto v = parse_string(bytes, pos, len);
            if (!v) return std::nullopt;
            out.hostname = std::move(*v);
            saw_hostname = true;
        } else if (*key == "tcp_port") {
            auto v = parse_uint(bytes, pos, len);
            if (!v || *v > 0xFFFFu) return std::nullopt;
            out.tcp_port = static_cast<std::uint16_t>(*v);
            saw_tcp_port = true;
        } else if (*key == "authed") {
            auto v = parse_bool(bytes, pos, len);
            if (!v) return std::nullopt;
            out.authed = *v;
        } else if (*key == "displays_csv") {
            auto v = parse_string(bytes, pos, len);
            if (!v) return std::nullopt;
            out.displays = decode_displays_csv(*v);
        } else {
            // Unknown key — tolerate by skipping the value.
            if (!skip_value(bytes, pos, len)) return std::nullopt;
        }

        skip_ws(bytes, pos, len);
        if (pos < len && bytes[pos] == ',') { ++pos; continue; }
        if (pos < len && bytes[pos] == '}') { ++pos; break; }
        return std::nullopt;
    }

    if (!saw_magic || !saw_machine_id || !saw_hostname || !saw_tcp_port) {
        return std::nullopt;
    }
    return out;
}

}  // namespace unio_ui::orchestrator::net
