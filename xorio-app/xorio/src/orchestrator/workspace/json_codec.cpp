/// @file json_codec.cpp
/// @brief Workspace-catalogue JSON encoder + decoder.
///
/// Single-pass recursive parser; no third-party JSON dep. Only
/// the public encode / decode functions are exported via
/// @ref json_codec.hpp; the parser class + helpers stay private
/// to this TU.

#include "orchestrator/workspace/json_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace xorio::orchestrator {

namespace {

void escape_json_into(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out.push_back(c); break;
        }
    }
    out.push_back('"');
}

class JsonParser {
public:
    explicit JsonParser(std::string_view s) : s_(s) {}

    bool ok() const { return !error_; }

    bool parse_workspaces(std::vector<Workspace>& out) {
        skip_ws();
        if (!consume('{')) return fail();
        // Walk top-level keys; only "workspaces" is consumed.
        while (true) {
            skip_ws();
            if (consume('}')) return true;
            std::string key;
            if (!parse_string(key)) return fail();
            skip_ws();
            if (!consume(':')) return fail();
            if (key == "workspaces") {
                if (!parse_array(out)) return fail();
            } else {
                if (!skip_value()) return fail();
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return fail();
        }
    }

private:
    bool parse_array(std::vector<Workspace>& out) {
        skip_ws();
        if (!consume('[')) return fail();
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            Workspace ws;
            if (!parse_workspace_obj(ws)) return false;
            out.push_back(std::move(ws));
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) return true;
            return fail();
        }
    }

    bool parse_workspace_obj(Workspace& ws) {
        skip_ws();
        if (!consume('{')) return fail();
        while (true) {
            skip_ws();
            if (consume('}')) return true;
            std::string key;
            if (!parse_string(key)) return fail();
            skip_ws();
            if (!consume(':')) return fail();
            if (key == "id") {
                if (!parse_string(ws.id)) return fail();
            } else if (key == "name") {
                if (!parse_string(ws.name)) return fail();
            } else if (key == "version_ns") {
                if (!parse_uint(ws.version_ns)) return fail();
            } else if (key == "tombstone") {
                if (!parse_bool(ws.tombstone)) return fail();
            } else if (key == "member_stamps") {
                if (!parse_member_stamps_array(ws.member_stamps)) return fail();
            } else if (key == "input_member_stamps") {
                if (!parse_member_stamps_array(ws.input_member_stamps)) return fail();
            } else if (key == "clipboard_member_stamps") {
                if (!parse_member_stamps_array(ws.clipboard_member_stamps)) return fail();
            } else if (key == "members") {
                // Legacy field — older saved files only carried the
                // union. Read it as-is; if input_members /
                // clipboard_members are also present they overwrite.
                std::vector<std::string> mems;
                if (!parse_string_array(mems)) return fail();
                ws.members.clear();
                for (auto& m : mems) ws.members.insert(std::move(m));
            } else if (key == "input_members") {
                std::vector<std::string> mems;
                if (!parse_string_array(mems)) return fail();
                ws.input_members.clear();
                for (auto& m : mems) ws.input_members.insert(std::move(m));
            } else if (key == "keyboard_members") {
                // Legacy field from when cursor + keyboard had
                // separate per-peer toggles. Read into
                // input_members so a saved workspace from before
                // the merge keeps both capabilities for any peer
                // that had keyboard ticked.
                std::vector<std::string> mems;
                if (!parse_string_array(mems)) return fail();
                for (auto& m : mems) ws.input_members.insert(std::move(m));
            } else if (key == "clipboard_members") {
                std::vector<std::string> mems;
                if (!parse_string_array(mems)) return fail();
                ws.clipboard_members.clear();
                for (auto& m : mems) ws.clipboard_members.insert(std::move(m));
            } else if (key == "clipboard_max") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return fail();
                if (v <= static_cast<unsigned>(ClipboardLimit::Unlimited)) {
                    ws.clipboard_max = static_cast<ClipboardLimit>(v);
                }
            } else if (key == "clipboard_rich") {
                if (!parse_bool(ws.clipboard_rich)) return fail();
            } else if (key == "clipboard_files") {
                if (!parse_bool(ws.clipboard_files)) return fail();
            } else if (key == "cursor_edge_margin") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return fail();
                ws.cursor_edge_margin = static_cast<std::int32_t>(v);
            } else if (key == "cursor_require_modifier") {
                if (!parse_bool(ws.cursor_require_modifier)) return fail();
            } else if (key == "cursor_block_hotkeys") {
                if (!parse_bool(ws.cursor_block_hotkeys)) return fail();
            } else if (key == "auto_unlock") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return fail();
                if (v <= static_cast<unsigned>(AutoUnlock::Hour1)) {
                    ws.auto_unlock = static_cast<AutoUnlock>(v);
                }
            } else if (key == "locked") {
                if (!parse_bool(ws.locked)) return fail();
            } else if (key == "lock_unlock_after_h") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return fail();
                ws.lock_unlock_after_h = static_cast<std::uint32_t>(v);
            } else if (key == "master_locked") {
                if (!parse_bool(ws.master_locked)) return fail();
            } else if (key == "master_lock_unlock_after_h") {
                std::uint64_t v = 0;
                if (!parse_uint(v)) return fail();
                ws.master_lock_unlock_after_h = static_cast<std::uint32_t>(v);
            } else if (key == "master_locked_by") {
                if (!parse_string(ws.master_locked_by)) return fail();
            } else if (key == "layout") {
                if (!parse_layout_array(ws.layout)) return fail();
            } else {
                if (!skip_value()) return fail();
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return fail();
        }
    }

    bool parse_string_array(std::vector<std::string>& out) {
        skip_ws();
        if (!consume('[')) return fail();
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            std::string item;
            if (!parse_string(item)) return fail();
            out.push_back(std::move(item));
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) return true;
            return fail();
        }
    }

    bool parse_member_stamps_array(
            std::unordered_map<std::string, MemberStamp>& out) {
        skip_ws();
        if (!consume('[')) return fail();
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            std::string mid;
            MemberStamp stamp;
            if (!parse_member_stamp_obj(mid, stamp)) return false;
            if (!mid.empty()) out.emplace(std::move(mid), stamp);
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) return true;
            return fail();
        }
    }

    bool parse_member_stamp_obj(std::string& mid, MemberStamp& stamp) {
        skip_ws();
        if (!consume('{')) return fail();
        while (true) {
            skip_ws();
            if (consume('}')) return true;
            std::string key;
            if (!parse_string(key)) return fail();
            skip_ws();
            if (!consume(':')) return fail();
            if (key == "machine_id") {
                if (!parse_string(mid)) return fail();
            } else if (key == "is_member") {
                if (!parse_bool(stamp.is_member)) return fail();
            } else if (key == "logical_clock") {
                if (!parse_uint(stamp.logical_clock)) return fail();
            } else {
                if (!skip_value()) return fail();
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return fail();
        }
    }

    bool parse_layout_array(std::vector<DisplayLayoutEntry>& out) {
        skip_ws();
        if (!consume('[')) return fail();
        skip_ws();
        if (consume(']')) return true;
        while (true) {
            DisplayLayoutEntry e;
            if (!parse_layout_entry(e)) return fail();
            out.push_back(std::move(e));
            skip_ws();
            if (consume(',')) continue;
            if (consume(']')) return true;
            return fail();
        }
    }

    bool parse_layout_entry(DisplayLayoutEntry& e) {
        skip_ws();
        if (!consume('{')) return fail();
        while (true) {
            skip_ws();
            if (consume('}')) return true;
            std::string key;
            if (!parse_string(key)) return fail();
            skip_ws();
            if (!consume(':')) return fail();
            if (key == "machine_id") {
                if (!parse_string(e.machine_id)) return fail();
            } else if (key == "monitor_id") {
                if (!parse_string(e.monitor_id)) return fail();
            } else if (key == "global_x") {
                std::uint64_t v = 0;
                bool neg = false;
                skip_ws();
                if (pos_ < s_.size() && s_[pos_] == '-') { neg = true; ++pos_; }
                if (!parse_uint(v)) return fail();
                e.global_x = neg ? -static_cast<std::int32_t>(v)
                                 :  static_cast<std::int32_t>(v);
            } else if (key == "global_y") {
                std::uint64_t v = 0;
                bool neg = false;
                skip_ws();
                if (pos_ < s_.size() && s_[pos_] == '-') { neg = true; ++pos_; }
                if (!parse_uint(v)) return fail();
                e.global_y = neg ? -static_cast<std::int32_t>(v)
                                 :  static_cast<std::int32_t>(v);
            } else {
                if (!skip_value()) return fail();
            }
            skip_ws();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return fail();
        }
    }

    bool parse_string(std::string& out) {
        skip_ws();
        if (!consume('"')) return fail();
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return fail();
                char e = s_[pos_++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'n':  out.push_back('\n'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'r':  out.push_back('\r'); break;
                    default:   out.push_back(e);    break;
                }
            } else {
                out.push_back(c);
            }
        }
        return fail();
    }

    bool parse_uint(std::uint64_t& out) {
        skip_ws();
        std::size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (pos_ == start) return fail();
        out = 0;
        for (std::size_t i = start; i < pos_; ++i) {
            out = out * 10 + static_cast<std::uint64_t>(s_[i] - '0');
        }
        return true;
    }

    bool parse_bool(bool& out) {
        skip_ws();
        if (pos_ + 4 <= s_.size()
            && s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4; out = true;  return true;
        }
        if (pos_ + 5 <= s_.size()
            && s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5; out = false; return true;
        }
        return fail();
    }

    bool skip_value() {
        skip_ws();
        if (pos_ >= s_.size()) return fail();
        char c = s_[pos_];
        if (c == '"') { std::string x; return parse_string(x); }
        if (c == 't' || c == 'f') { bool b; return parse_bool(b); }
        if (c == '-' || (c >= '0' && c <= '9')) {
            if (c == '-') ++pos_;
            std::uint64_t x; return parse_uint(x);
        }
        if (c == 'n' && pos_ + 4 <= s_.size()
            && s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4; return true;
        }
        if (c == '[') {
            ++pos_;
            int depth = 1;
            while (pos_ < s_.size() && depth > 0) {
                char d = s_[pos_++];
                if (d == '"') {
                    --pos_;
                    std::string x;
                    if (!parse_string(x)) return fail();
                } else if (d == '[' || d == '{') ++depth;
                else if (d == ']' || d == '}') --depth;
            }
            return depth == 0;
        }
        if (c == '{') {
            ++pos_;
            int depth = 1;
            while (pos_ < s_.size() && depth > 0) {
                char d = s_[pos_++];
                if (d == '"') {
                    --pos_;
                    std::string x;
                    if (!parse_string(x)) return fail();
                } else if (d == '[' || d == '{') ++depth;
                else if (d == ']' || d == '}') --depth;
            }
            return depth == 0;
        }
        return fail();
    }

    void skip_ws() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool consume(char c) {
        if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
        return false;
    }

    bool fail() { error_ = true; return false; }

    std::string_view s_;
    std::size_t      pos_   = 0;
    bool             error_ = false;
};

}  // namespace

std::string encode_workspaces_json(const std::vector<Workspace>& items) {
    std::string out;
    out.reserve(256 + 192 * items.size());
    out += R"({"workspaces":[)";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out.push_back(',');
        const auto& ws = items[i];
        out += R"({"id":)";       escape_json_into(out, ws.id);
        out += R"(,"name":)";     escape_json_into(out, ws.name);
        out += R"(,"version_ns":)";
        out += std::to_string(ws.version_ns);
        out += R"(,"tombstone":)";
        out += ws.tombstone ? "true" : "false";
        // Per-PC LWW stamp maps — the new source of truth for
        // membership and per-cap on/off. The flat sets below
        // (`members`, `input_members`, `clipboard_members`) are
        // kept derived for back-compat with older readers / older
        // saved files.
        auto emit_stamps = [&](const char* key,
                               const std::unordered_map<std::string,
                                                        MemberStamp>& m) {
            out += ',';
            out += '"';
            out += key;
            out += R"(":[)";
            std::vector<std::string> keys;
            keys.reserve(m.size());
            for (const auto& [mid, _] : m) keys.push_back(mid);
            std::sort(keys.begin(), keys.end());
            for (std::size_t k = 0; k < keys.size(); ++k) {
                if (k != 0) out.push_back(',');
                const auto& mid = keys[k];
                const auto& st  = m.at(mid);
                out += R"({"machine_id":)";
                escape_json_into(out, mid);
                out += R"(,"is_member":)";
                out += st.is_member ? "true" : "false";
                out += R"(,"logical_clock":)";
                out += std::to_string(st.logical_clock);
                out += "}";
            }
            out += "]";
        };
        emit_stamps("member_stamps",            ws.member_stamps);
        emit_stamps("input_member_stamps",      ws.input_member_stamps);
        emit_stamps("clipboard_member_stamps",  ws.clipboard_member_stamps);
        // Legacy `members` flat array — kept on the wire / disk so
        // peers running pre-stamp builds still see the right
        // membership. Newer readers ignore this when stamps are
        // present.
        out += R"(,"members":[)";
        std::vector<std::string> sorted_mem(
            ws.members.begin(), ws.members.end());
        std::sort(sorted_mem.begin(), sorted_mem.end());
        for (std::size_t k = 0; k < sorted_mem.size(); ++k) {
            if (k != 0) out.push_back(',');
            escape_json_into(out, sorted_mem[k]);
        }
        out += "]";
        out += R"(,"input_members":[)";
        std::vector<std::string> sorted_in(
            ws.input_members.begin(), ws.input_members.end());
        std::sort(sorted_in.begin(), sorted_in.end());
        for (std::size_t k = 0; k < sorted_in.size(); ++k) {
            if (k != 0) out.push_back(',');
            escape_json_into(out, sorted_in[k]);
        }
        out += "]";
        out += R"(,"clipboard_members":[)";
        std::vector<std::string> sorted_cb(
            ws.clipboard_members.begin(), ws.clipboard_members.end());
        std::sort(sorted_cb.begin(), sorted_cb.end());
        for (std::size_t k = 0; k < sorted_cb.size(); ++k) {
            if (k != 0) out.push_back(',');
            escape_json_into(out, sorted_cb[k]);
        }
        out += "]";
        // Settings — clipboard / cursor / auto-unlock. New fields;
        // the parser tolerates their absence in older files by
        // leaving struct defaults in place.
        out += R"(,"clipboard_max":)";
        out += std::to_string(static_cast<unsigned>(ws.clipboard_max));
        out += R"(,"clipboard_rich":)";
        out += ws.clipboard_rich ? "true" : "false";
        out += R"(,"clipboard_files":)";
        out += ws.clipboard_files ? "true" : "false";
        out += R"(,"cursor_edge_margin":)";
        out += std::to_string(ws.cursor_edge_margin);
        out += R"(,"cursor_require_modifier":)";
        out += ws.cursor_require_modifier ? "true" : "false";
        out += R"(,"cursor_block_hotkeys":)";
        out += ws.cursor_block_hotkeys ? "true" : "false";
        out += R"(,"auto_unlock":)";
        out += std::to_string(static_cast<unsigned>(ws.auto_unlock));
        out += R"(,"locked":)";
        out += ws.locked ? "true" : "false";
        out += R"(,"lock_unlock_after_h":)";
        out += std::to_string(ws.lock_unlock_after_h);
        out += R"(,"master_locked":)";
        out += ws.master_locked ? "true" : "false";
        out += R"(,"master_lock_unlock_after_h":)";
        out += std::to_string(ws.master_lock_unlock_after_h);
        out += R"(,"master_locked_by":)";
        escape_json_into(out, ws.master_locked_by);
        out += R"(,"layout":[)";
        for (std::size_t k = 0; k < ws.layout.size(); ++k) {
            if (k != 0) out.push_back(',');
            const auto& e = ws.layout[k];
            out += R"({"machine_id":)";
            escape_json_into(out, e.machine_id);
            out += R"(,"monitor_id":)";
            escape_json_into(out, e.monitor_id);
            out += R"(,"global_x":)";
            out += std::to_string(e.global_x);
            out += R"(,"global_y":)";
            out += std::to_string(e.global_y);
            out += "}";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

bool decode_workspaces_json(std::string_view text,
                             std::vector<Workspace>& out) {
    JsonParser p(text);
    return p.parse_workspaces(out) && p.ok();
}

}  // namespace xorio::orchestrator
