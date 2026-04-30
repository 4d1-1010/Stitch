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

namespace xorio::orchestrator::net {

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

// ── workspaces_v1 sub-payload ──────────────────────────────────
//
// Length-prefixed binary-style format inside a single JSON string
// field. Avoids any in-string escape gymnastics — strings (workspace
// names, machine_ids) are written raw, with their byte length given
// up front. Layout (newline-separated for human-readability when
// inspected):
//
//   <ws_count>\n
//   <id_len>\n<id>
//   <name_len>\n<name>
//   <version_ns>\n
//   <tombstone 0|1>\n
//   <member_count>\n
//   <m1_len>\n<m1><m2_len>\n<m2>...
//   …repeat per workspace…
//
// All numeric fields are decimal ASCII. The format carries no
// magic — the surrounding JSON field name `workspaces_v1` is the
// version marker.

void append_uint(std::string& out, std::uint64_t v) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

std::string encode_workspaces_v1(const std::vector<AnnounceWorkspace>& ws) {
    std::string out;
    out.reserve(96 + 128 * ws.size());
    append_uint(out, ws.size()); out.push_back('\n');
    for (const auto& w : ws) {
        append_uint(out, w.id.size());      out.push_back('\n');
        out += w.id;
        append_uint(out, w.name.size());    out.push_back('\n');
        out += w.name;
        append_uint(out, w.version_ns);     out.push_back('\n');
        out.push_back(w.tombstone ? '1' : '0'); out.push_back('\n');
        append_uint(out, w.members.size()); out.push_back('\n');
        for (const auto& m : w.members) {
            append_uint(out, m.size());     out.push_back('\n');
            out += m;
        }
        // Per-capability member sets. Length-prefixed lists, same
        // shape as the union list above.
        append_uint(out, w.input_members.size()); out.push_back('\n');
        for (const auto& m : w.input_members) {
            append_uint(out, m.size()); out.push_back('\n');
            out += m;
        }
        append_uint(out, w.clipboard_members.size()); out.push_back('\n');
        for (const auto& m : w.clipboard_members) {
            append_uint(out, m.size()); out.push_back('\n');
            out += m;
        }
        // Settings block — fixed layout, same order as the
        // decoder reads. Newline-separated decimal ints / 0|1
        // flags so they pass the same read_uint_until_newline
        // helper used for the rest of the record.
        append_uint(out, w.clipboard_max);  out.push_back('\n');
        out.push_back(w.clipboard_rich  ? '1' : '0'); out.push_back('\n');
        out.push_back(w.clipboard_files ? '1' : '0'); out.push_back('\n');
        // Edge margin is a signed pixel count but always
        // non-negative in the UI; encode as uint to share the
        // helper.
        append_uint(out, static_cast<std::uint64_t>(
            w.cursor_edge_margin < 0 ? 0 : w.cursor_edge_margin));
        out.push_back('\n');
        out.push_back(w.cursor_require_modifier ? '1' : '0');
        out.push_back('\n');
        out.push_back(w.cursor_block_hotkeys ? '1' : '0');
        out.push_back('\n');
        append_uint(out, w.auto_unlock);    out.push_back('\n');

        // Layout entries — per-monitor user-arranged positions.
        // Encoded as <count> followed by per-entry length-prefixed
        // strings + decimal coords (negative coords use a leading
        // minus sign captured in read_lp_string's caller).
        append_uint(out, w.layout.size()); out.push_back('\n');
        for (const auto& e : w.layout) {
            append_uint(out, e.machine_id.size()); out.push_back('\n');
            out += e.machine_id;
            append_uint(out, e.monitor_id.size()); out.push_back('\n');
            out += e.monitor_id;
            // global_x/y can be negative — append the signed
            // decimal directly so the decoder's read_int_until
            // helper handles the sign cleanly.
            out += std::to_string(e.global_x); out.push_back('\n');
            out += std::to_string(e.global_y); out.push_back('\n');
        }

        // Per-PC LWW stamps — trailing block, optional. Older
        // receivers stop reading after the layout block and
        // synthesize clock=0 stamps from the legacy `members`
        // list, so a stamp emitted here always wins via LWW.
        append_uint(out, w.member_stamps.size()); out.push_back('\n');
        for (const auto& s : w.member_stamps) {
            append_uint(out, s.machine_id.size()); out.push_back('\n');
            out += s.machine_id;
            out.push_back(s.is_member ? '1' : '0'); out.push_back('\n');
            append_uint(out, s.logical_clock);      out.push_back('\n');
        }

        // Lock state — trailing block, optional. Same forgiving
        // tolerance as the membership block: older senders stop
        // here, receivers default the lock fields to "not locked".
        out.push_back(w.locked ? '1' : '0'); out.push_back('\n');
        append_uint(out, w.lock_unlock_after_h); out.push_back('\n');
        out.push_back(w.master_locked ? '1' : '0'); out.push_back('\n');
        append_uint(out, w.master_lock_unlock_after_h); out.push_back('\n');
        append_uint(out, w.master_locked_by.size()); out.push_back('\n');
        out += w.master_locked_by;
    }
    return out;
}

/// @brief Read a decimal uint terminated by the next `\n`. Advances
/// @p pos past the newline. Returns false if no digits are found.
bool read_uint_until_newline(std::string_view s, std::size_t& pos,
                              std::uint64_t& out) {
    std::size_t start = pos;
    while (pos < s.size() && s[pos] != '\n') ++pos;
    if (pos == start) return false;
    auto r = std::from_chars(s.data() + start, s.data() + pos, out);
    if (r.ec != std::errc{}) return false;
    if (pos >= s.size()) return false;
    ++pos;  // consume the newline.
    return true;
}

/// @brief Read a length-prefixed raw byte string: `<len>\n<bytes>`.
bool read_lp_string(std::string_view s, std::size_t& pos, std::string& out) {
    std::uint64_t len = 0;
    if (!read_uint_until_newline(s, pos, len)) return false;
    if (pos + len > s.size()) return false;
    out.assign(s.data() + pos, len);
    pos += static_cast<std::size_t>(len);
    return true;
}

/// @brief Read a signed decimal integer terminated by `\n`.
/// Used for layout coordinates which can be negative when the
/// user drags a monitor left of the origin.
bool read_int_until_newline(std::string_view s, std::size_t& pos,
                              std::int32_t& out) {
    std::size_t start = pos;
    if (pos < s.size() && s[pos] == '-') ++pos;
    while (pos < s.size() && s[pos] != '\n') ++pos;
    if (pos == start) return false;
    long long v = 0;
    auto r = std::from_chars(s.data() + start, s.data() + pos, v);
    if (r.ec != std::errc{}) return false;
    if (pos >= s.size()) return false;
    ++pos;
    out = static_cast<std::int32_t>(v);
    return true;
}

std::vector<AnnounceWorkspace> decode_workspaces_v1(std::string_view s) {
    std::vector<AnnounceWorkspace> out;
    std::size_t pos = 0;
    std::uint64_t count = 0;
    if (!read_uint_until_newline(s, pos, count)) return out;
    out.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        AnnounceWorkspace w;
        if (!read_lp_string(s, pos, w.id))   return {};
        if (!read_lp_string(s, pos, w.name)) return {};
        std::uint64_t v_ns = 0;
        if (!read_uint_until_newline(s, pos, v_ns)) return {};
        w.version_ns = v_ns;
        if (pos >= s.size()) return {};
        const char tomb = s[pos++];
        if (pos >= s.size() || s[pos] != '\n') return {};
        ++pos;
        w.tombstone = (tomb == '1');
        std::uint64_t mc = 0;
        if (!read_uint_until_newline(s, pos, mc)) return {};
        w.members.reserve(static_cast<std::size_t>(mc));
        for (std::uint64_t k = 0; k < mc; ++k) {
            std::string m;
            if (!read_lp_string(s, pos, m)) return {};
            w.members.push_back(std::move(m));
        }

        // Per-capability member sets — fixed [input][clipboard]
        // layout, written by encode_workspaces_v1 between the
        // union list and the settings block. (An earlier version
        // of this codec carried a third [keyboard] list; that
        // collapsed into [input] when the form merged Cursor and
        // Keyboard into a single Input checkbox. Probe-then-fall-
        // back disambiguation against the older 3-list layout
        // was unsafe: when a 2-list payload happened to follow
        // with valid-looking integers in the settings block, the
        // probe would consume those as if they were a third
        // member list and then mis-parse the rest of the record.
        // Both peers run the same build, so we just commit to
        // the 2-list format here.) Tolerated as missing for
        // backward compat with any older sender that doesn't
        // carry caps at all.
        const std::size_t caps_start = pos;
        std::uint64_t ic = 0;
        if (read_uint_until_newline(s, pos, ic)) {
            w.input_members.reserve(static_cast<std::size_t>(ic));
            bool ok = true;
            for (std::uint64_t k = 0; k < ic; ++k) {
                std::string m;
                if (!read_lp_string(s, pos, m)) { ok = false; break; }
                w.input_members.push_back(std::move(m));
            }
            std::uint64_t cc = 0;
            if (!ok || !read_uint_until_newline(s, pos, cc)) {
                pos = caps_start;
                w.input_members.clear();
            } else {
                w.clipboard_members.reserve(
                    static_cast<std::size_t>(cc));
                for (std::uint64_t k = 0; k < cc; ++k) {
                    std::string m;
                    if (!read_lp_string(s, pos, m)) { ok = false; break; }
                    w.clipboard_members.push_back(std::move(m));
                }
                if (!ok) {
                    pos = caps_start;
                    w.input_members.clear();
                    w.clipboard_members.clear();
                }
            }
        } else {
            pos = caps_start;
        }

        // Settings block — same fixed order as the encoder. Bail
        // out cleanly if the sender used the older v1 layout (no
        // settings); just default the fields.
        const std::size_t settings_start = pos;
        std::uint64_t v = 0;
        bool ok = read_uint_until_newline(s, pos, v);
        if (!ok) {
            // Older sender — keep defaults, advance past nothing.
            pos = settings_start;
        } else {
            w.clipboard_max = static_cast<std::uint8_t>(v);
            // The remaining 6 fields must all be present once we
            // entered the settings block; treat missing as bug.
            if (pos >= s.size() || (s[pos] != '0' && s[pos] != '1')) return {};
            w.clipboard_rich = (s[pos] == '1'); pos += 2; // digit + '\n'
            if (pos >= s.size()) return {};
            w.clipboard_files = (s[pos] == '1'); pos += 2;
            std::uint64_t edge = 0;
            if (!read_uint_until_newline(s, pos, edge)) return {};
            w.cursor_edge_margin = static_cast<std::int32_t>(edge);
            if (pos >= s.size()) return {};
            w.cursor_require_modifier = (s[pos] == '1'); pos += 2;
            if (pos >= s.size()) return {};
            w.cursor_block_hotkeys = (s[pos] == '1'); pos += 2;
            std::uint64_t au = 0;
            if (!read_uint_until_newline(s, pos, au)) return {};
            w.auto_unlock = static_cast<std::uint8_t>(au);

            // Layout entries — same tolerance as the settings
            // block: missing trailing data is OK, partial data
            // means an older sender we don't expect mid-rollout.
            const std::size_t layout_start = pos;
            std::uint64_t lc = 0;
            if (read_uint_until_newline(s, pos, lc)) {
                w.layout.reserve(static_cast<std::size_t>(lc));
                bool layout_ok = true;
                for (std::uint64_t k = 0; k < lc; ++k) {
                    AnnounceWorkspace::LayoutEntry e;
                    if (!read_lp_string(s, pos, e.machine_id))   { layout_ok = false; break; }
                    if (!read_lp_string(s, pos, e.monitor_id))   { layout_ok = false; break; }
                    if (!read_int_until_newline(s, pos, e.global_x)) { layout_ok = false; break; }
                    if (!read_int_until_newline(s, pos, e.global_y)) { layout_ok = false; break; }
                    w.layout.push_back(std::move(e));
                }
                if (!layout_ok) {
                    pos = layout_start;
                    w.layout.clear();
                }
            } else {
                pos = layout_start;
            }

            // Per-PC member stamps — same tolerance as the layout
            // block: missing trailing data means an older sender
            // and we just leave member_stamps empty (the receiver
            // will synthesize clock=0 stamps from `members`).
            const std::size_t stamps_start = pos;
            std::uint64_t sc = 0;
            if (read_uint_until_newline(s, pos, sc)) {
                w.member_stamps.reserve(static_cast<std::size_t>(sc));
                bool stamps_ok = true;
                for (std::uint64_t k = 0; k < sc; ++k) {
                    AnnounceWorkspace::MemberStamp st;
                    if (!read_lp_string(s, pos, st.machine_id)) {
                        stamps_ok = false; break;
                    }
                    if (pos >= s.size() || (s[pos] != '0' && s[pos] != '1')) {
                        stamps_ok = false; break;
                    }
                    st.is_member = (s[pos] == '1');
                    pos += 2;  // digit + '\n'
                    if (!read_uint_until_newline(s, pos, st.logical_clock)) {
                        stamps_ok = false; break;
                    }
                    w.member_stamps.push_back(std::move(st));
                }
                if (!stamps_ok) {
                    pos = stamps_start;
                    w.member_stamps.clear();
                }
            } else {
                pos = stamps_start;
            }

            // Lock state — same forgiving tolerance.
            const std::size_t lock_start = pos;
            if (pos < s.size() && (s[pos] == '0' || s[pos] == '1')) {
                bool locked_in        = (s[pos] == '1'); pos += 2;
                std::uint64_t lock_h  = 0;
                bool master_in        = false;
                std::uint64_t mlock_h = 0;
                std::string master_by;
                bool lock_ok = read_uint_until_newline(s, pos, lock_h);
                if (lock_ok && pos < s.size()
                    && (s[pos] == '0' || s[pos] == '1')) {
                    master_in = (s[pos] == '1'); pos += 2;
                    lock_ok = read_uint_until_newline(s, pos, mlock_h);
                    if (lock_ok) lock_ok = read_lp_string(s, pos, master_by);
                } else {
                    lock_ok = false;
                }
                if (lock_ok) {
                    w.locked                     = locked_in;
                    w.lock_unlock_after_h        =
                        static_cast<std::uint32_t>(lock_h);
                    w.master_locked              = master_in;
                    w.master_lock_unlock_after_h =
                        static_cast<std::uint32_t>(mlock_h);
                    w.master_locked_by           = std::move(master_by);
                } else {
                    pos = lock_start;
                }
            } else {
                pos = lock_start;
            }
        }
        out.push_back(std::move(w));
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

    // Optional data_port — omitted when 0 so older receivers
    // that don't know the field still parse cleanly. Carried
    // alongside tcp_port; both ports listened on by the same
    // peer process.
    if (p.data_port != 0) {
        append_str(out, R"(,"data_port":)");
        char dp_buf[8];
        const auto dr = std::to_chars(dp_buf, dp_buf + sizeof(dp_buf),
                                       p.data_port);
        out.insert(out.end(),
                   reinterpret_cast<const std::uint8_t*>(dp_buf),
                   reinterpret_cast<const std::uint8_t*>(dr.ptr));
    }

    // Optional `displays_csv` extension. Omitted entirely when
    // empty so wire-format-compatible Python decoders that don't
    // know the field don't see an unexpected key — the C++
    // decoder accepts both shapes.
    if (!p.displays.empty()) {
        append_str(out, R"(,"displays_csv":)");
        append_json_string(out, encode_displays_csv(p.displays));
    }
    // Optional identify-request counter. Omitted while 0 so an
    // unbumped peer's announce stays byte-identical to the older
    // schema.
    if (p.identify_request_id != 0) {
        append_str(out, R"(,"identify_request_id":)");
        char num_buf[24];
        const auto rc = std::to_chars(num_buf, num_buf + sizeof(num_buf),
                                       p.identify_request_id);
        out.insert(out.end(),
                   reinterpret_cast<const std::uint8_t*>(num_buf),
                   reinterpret_cast<const std::uint8_t*>(rc.ptr));
    }
    // Optional `workspaces_v1` extension — the announcer's full
    // local workspace catalogue. Omitted entirely when empty so
    // pre-extension receivers still parse cleanly.
    if (!p.workspaces.empty()) {
        append_str(out, R"(,"workspaces_v1":)");
        append_json_string(out, encode_workspaces_v1(p.workspaces));
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
        } else if (*key == "data_port") {
            auto v = parse_uint(bytes, pos, len);
            if (!v || *v > 0xFFFFu) return std::nullopt;
            out.data_port = static_cast<std::uint16_t>(*v);
        } else if (*key == "authed") {
            auto v = parse_bool(bytes, pos, len);
            if (!v) return std::nullopt;
            out.authed = *v;
        } else if (*key == "displays_csv") {
            auto v = parse_string(bytes, pos, len);
            if (!v) return std::nullopt;
            out.displays = decode_displays_csv(*v);
        } else if (*key == "identify_request_id") {
            auto v = parse_uint(bytes, pos, len);
            if (!v) return std::nullopt;
            out.identify_request_id = *v;
        } else if (*key == "workspaces_v1") {
            auto v = parse_string(bytes, pos, len);
            if (!v) return std::nullopt;
            out.workspaces = decode_workspaces_v1(*v);
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

}  // namespace xorio::orchestrator::net
