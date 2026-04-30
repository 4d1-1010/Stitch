/// @file manager.cpp
/// @brief Local @ref IWorkspaceManager with last-writer-wins remote
/// merge + JSON persistence.
///
/// Catalogue state is mutex-protected; the orchestrator may call
/// list() from the UI thread while a worker thread services a
/// mutation. Every mutation bumps a Lamport-style `version_ns`
/// counter and writes the catalogue to disk synchronously, so
/// state survives a process restart.
///
/// Mesh propagation is decoupled: the orchestrator broadcasts the
/// catalogue (via @ref wire_state) on each LAN announce; on
/// receive, the same orchestrator hands the remote catalogue to
/// @ref merge_remote which applies LWW per-id.

#include "orchestrator/workspace.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#  include <direct.h>
#  include <windows.h>
#  include <shlobj.h>
#else
#  include <pwd.h>
#  include <unistd.h>
#endif

namespace xorio::orchestrator {

namespace {

/// @brief Steady wall-clock nanoseconds since the UNIX epoch — used
/// as the Lamport-style version stamp on every mutation. Two PCs
/// whose clocks are seconds apart still converge correctly because
/// merge() compares only via `>` per-record; the absolute value
/// matters only as a tie-break, not as truth.
std::uint64_t now_ns() {
    using clk = std::chrono::system_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clk::now().time_since_epoch()).count());
}

/// @brief Generate a stable opaque id. Suffixed with a timestamp +
/// per-process counter so two PCs creating workspaces at the same
/// instant don't collide.
std::string generate_id() {
    static std::atomic<std::uint64_t> counter{0};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ws-%llu-%llu",
                  static_cast<unsigned long long>(now_ns()),
                  static_cast<unsigned long long>(++counter));
    return buf;
}

/// @brief Resolve the on-disk catalogue file path:
///   * Linux:   $XDG_CONFIG_HOME/xorio/workspaces.json,
///              fallback to $HOME/.config/xorio/workspaces.json
///   * Windows: %APPDATA%/xorio/workspaces.json
/// Creates the parent directory if missing. Returns empty string
/// when the home/appdata can't be resolved (read-only fallback).
std::string config_dir() {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, buf) == S_OK) {
        std::string path = std::string(buf) + "\\xorio";
        ::CreateDirectoryA(path.c_str(), nullptr);
        return path;
    }
    return {};
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0] != '\0') {
        base = std::string(xdg) + "/xorio";
    } else {
        const char* home = std::getenv("HOME");
        if (!home) {
            if (auto* pw = ::getpwuid(::getuid())) home = pw->pw_dir;
        }
        if (!home) return {};
        base = std::string(home) + "/.config/xorio";
    }
    ::mkdir(base.c_str(), 0700);
    return base;
#endif
}

std::string catalogue_path() {
    std::string dir = config_dir();
    if (dir.empty()) return {};
#if defined(_WIN32)
    return dir + "\\workspaces.json";
#else
    return dir + "/workspaces.json";
#endif
}

// ── Tiny JSON helpers ─────────────────────────────────────────
//
// We deliberately don't pull in a JSON dependency. The shape we
// persist is fixed:
//
//   {"workspaces":[
//      {"id":"…","name":"…","version_ns":N,"tombstone":false,
//       "members":["…","…"]},
//      …
//   ]}
//
// Encoder is straightforward; decoder is a single-pass recursive
// parser that handles enough JSON for this fixed shape.

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

/// @brief Encode a workspace catalogue (including tombstones) as
/// a single JSON document — used both for persistence on disk and
/// for the in-process broadcast over LAN announces.
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
        // Per-PC LWW stamps — the new source of truth for
        // membership. `members` is kept derived for back-compat
        // with older readers / older saved files.
        out += R"(,"member_stamps":[)";
        std::vector<std::string> sorted_keys;
        sorted_keys.reserve(ws.member_stamps.size());
        for (const auto& [mid, _] : ws.member_stamps) sorted_keys.push_back(mid);
        std::sort(sorted_keys.begin(), sorted_keys.end());
        for (std::size_t k = 0; k < sorted_keys.size(); ++k) {
            if (k != 0) out.push_back(',');
            const auto& mid = sorted_keys[k];
            const auto& st  = ws.member_stamps.at(mid);
            out += R"({"machine_id":)";
            escape_json_into(out, mid);
            out += R"(,"is_member":)";
            out += st.is_member ? "true" : "false";
            out += R"(,"logical_clock":)";
            out += std::to_string(st.logical_clock);
            out += "}";
        }
        out += "]";
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

bool decode_workspaces_json(std::string_view text,
                             std::vector<Workspace>& out) {
    JsonParser p(text);
    return p.parse_workspaces(out) && p.ok();
}

class WorkspaceManager final : public IWorkspaceManager {
public:
    WorkspaceManager() {
        load_from_disk();
    }

    std::vector<Workspace> list() const override {
        std::lock_guard lk(m_);
        std::vector<Workspace> out;
        out.reserve(workspaces_.size());
        for (const auto& [_, ws] : workspaces_) {
            if (ws.tombstone) continue;
            out.push_back(ws);
        }
        std::sort(out.begin(), out.end(),
                  [](const Workspace& a, const Workspace& b) {
                      if (a.name != b.name) return a.name < b.name;
                      return a.id < b.id;
                  });
        return out;
    }

    std::optional<Workspace> get(const std::string& id) const override {
        std::lock_guard lk(m_);
        auto it = workspaces_.find(id);
        if (it == workspaces_.end() || it->second.tombstone) return std::nullopt;
        return it->second;
    }

    std::vector<std::pair<std::string, std::string>>
    pc_assignments() const override {
        std::lock_guard lk(m_);
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(workspaces_.size());
        for (const auto& [_, ws] : workspaces_) {
            if (ws.tombstone) continue;
            for (const auto& mid : ws.members) {
                out.emplace_back(mid, ws.id);
            }
        }
        return out;
    }

    std::string create(const std::string& name,
                       const std::unordered_set<std::string>& members,
                       const std::unordered_set<std::string>& input_members,
                       const std::unordered_set<std::string>& clipboard_members) override {
        std::string id = generate_id();
        {
            std::lock_guard lk(m_);
            evict_members_locked(members, /*except_id=*/{});
            Workspace ws;
            ws.id                 = id;
            ws.name               = name;
            ws.input_members      = clamp_to(input_members,    members);
            ws.clipboard_members  = clamp_to(clipboard_members, members);
            ws.version_ns         = now_ns();
            // Optimistic per-PC stamps for every initial member.
            // Each peer's own machine writes its own stamp later
            // (via leave) — those are the writes that actually own
            // the truth via LWW; this seeds the catalogue so the
            // workspace is alive on every peer's first announce.
            const std::uint64_t clk = now_ns();
            for (const auto& mid : members) {
                ws.member_stamps[mid] = MemberStamp{true, clk};
            }
            recompute_members_locked(ws);
            workspaces_.emplace(id, std::move(ws));
            save_locked();
        }
        notify(id);
        return id;
    }

    void rename(const std::string& id, const std::string& new_name) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it != workspaces_.end() && !it->second.tombstone
                && it->second.name != new_name) {
                it->second.name       = new_name;
                it->second.version_ns = now_ns();
                changed = true;
                save_locked();
            }
        }
        if (changed) notify(id);
    }

    void set_members(const std::string& id,
                     const std::unordered_set<std::string>& members,
                     const std::unordered_set<std::string>& input_members,
                     const std::unordered_set<std::string>& clipboard_members) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it == workspaces_.end() || it->second.tombstone) return;
            evict_members_locked(members, /*except_id=*/id);
            std::unordered_set<std::string> in_clamped =
                clamp_to(input_members, members);
            std::unordered_set<std::string> cb_clamped =
                clamp_to(clipboard_members, members);
            if (it->second.members           != members
                || it->second.input_members     != in_clamped
                || it->second.clipboard_members != cb_clamped) {
                // Update stamps for any membership delta. New
                // members get a fresh true-stamp; removed members
                // get a fresh false-stamp so the absence is
                // explicit on the wire (LWW peers can't tell
                // "missing key" from "never been a member"
                // otherwise).
                const std::uint64_t clk = now_ns();
                for (const auto& mid : members) {
                    auto& st = it->second.member_stamps[mid];
                    if (!st.is_member || st.logical_clock < clk) {
                        st = MemberStamp{true, clk};
                    }
                }
                for (const auto& mid : it->second.members) {
                    if (members.count(mid) == 0) {
                        auto& st = it->second.member_stamps[mid];
                        if (st.is_member || st.logical_clock < clk) {
                            st = MemberStamp{false, clk};
                        }
                    }
                }
                it->second.input_members     = std::move(in_clamped);
                it->second.clipboard_members = std::move(cb_clamped);
                it->second.version_ns        = now_ns();
                recompute_members_locked(it->second);
                changed = true;
                save_locked();
            }
        }
        if (changed) notify(id);
    }

    void leave(const std::string& id,
               const std::string& machine_id) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it == workspaces_.end() || it->second.tombstone) return;
            const std::uint64_t clk = now_ns();
            auto& st = it->second.member_stamps[machine_id];
            // Always advance the clock — even if is_member was
            // already false — so a re-leave after a remote re-add
            // sticks. Equal-clock ties bias toward false elsewhere
            // (in merge_remote) so this is also the strongest
            // "I've left" signal we can emit.
            st = MemberStamp{false, std::max(clk, st.logical_clock + 1)};
            recompute_members_locked(it->second);
            changed = true;
            save_locked();
        }
        if (changed) notify(id);
    }

    void set_layout(const std::string& id,
                     const std::vector<DisplayLayoutEntry>& layout) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it == workspaces_.end() || it->second.tombstone) return;
            if (it->second.layout != layout) {
                it->second.layout     = layout;
                it->second.version_ns = now_ns();
                changed = true;
                save_locked();
            }
        }
        if (changed) notify(id);
    }

    void set_settings(const std::string& id,
                      const WorkspaceSettings& s) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it == workspaces_.end() || it->second.tombstone) return;
            auto& ws = it->second;
            if (ws.clipboard_max          != s.clipboard_max
                || ws.clipboard_rich      != s.clipboard_rich
                || ws.clipboard_files     != s.clipboard_files
                || ws.cursor_edge_margin  != s.cursor_edge_margin
                || ws.cursor_require_modifier != s.cursor_require_modifier
                || ws.cursor_block_hotkeys != s.cursor_block_hotkeys
                || ws.auto_unlock         != s.auto_unlock) {
                ws.clipboard_max           = s.clipboard_max;
                ws.clipboard_rich          = s.clipboard_rich;
                ws.clipboard_files         = s.clipboard_files;
                ws.cursor_edge_margin      = s.cursor_edge_margin;
                ws.cursor_require_modifier = s.cursor_require_modifier;
                ws.cursor_block_hotkeys    = s.cursor_block_hotkeys;
                ws.auto_unlock             = s.auto_unlock;
                ws.version_ns              = now_ns();
                changed = true;
                save_locked();
            }
        }
        if (changed) notify(id);
    }

    void destroy(const std::string& id) override {
        bool removed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it != workspaces_.end() && !it->second.tombstone) {
                it->second.tombstone  = true;
                it->second.members.clear();
                it->second.input_members.clear();
                it->second.clipboard_members.clear();
                it->second.version_ns = now_ns();
                removed = true;
                save_locked();
            }
        }
        if (removed) notify(id);
    }

    void acquire_lock(const std::string& id,
                      const std::string& machine_id) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it != workspaces_.end() && !it->second.tombstone
                && it->second.locked_by != machine_id) {
                it->second.locked_by = machine_id;
                changed = true;
            }
        }
        if (changed) notify(id);
    }

    void release_lock(const std::string& id,
                      const std::string& machine_id) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it != workspaces_.end() && !it->second.tombstone
                && it->second.locked_by == machine_id) {
                it->second.locked_by.clear();
                changed = true;
            }
        }
        if (changed) notify(id);
    }

    std::vector<Workspace> wire_state() const override {
        std::lock_guard lk(m_);
        std::vector<Workspace> out;
        out.reserve(workspaces_.size());
        for (const auto& [_, ws] : workspaces_) out.push_back(ws);
        std::sort(out.begin(), out.end(),
                  [](const Workspace& a, const Workspace& b) {
                      return a.id < b.id;
                  });
        return out;
    }

    void merge_remote(const std::vector<Workspace>& remote) override {
        bool any_change = false;
        {
            std::lock_guard lk(m_);
            for (const auto& r_in : remote) {
                if (r_in.id.empty()) continue;
                // Older peers don't send `member_stamps` — synthesize
                // them from the legacy `members` set with clock=0
                // so any locally-stamped value (clock>=1) wins.
                Workspace r = r_in;
                if (r.member_stamps.empty() && !r.members.empty()) {
                    for (const auto& mid : r.members) {
                        r.member_stamps[mid] = MemberStamp{true, 0};
                    }
                }
                auto it = workspaces_.find(r.id);
                if (it == workspaces_.end()) {
                    if (r.tombstone) continue;  // nothing to delete locally.
                    Workspace ws = std::move(r);
                    recompute_members_locked(ws);
                    workspaces_.emplace(ws.id, std::move(ws));
                    any_change = true;
                    continue;
                }
                Workspace& local = it->second;
                bool local_changed = false;
                // Per-stamp LWW for membership: each machine's
                // stamp merges by logical_clock; ties bias toward
                // is_member=false so a leave can't be undone by a
                // stale "you're in" stamp from the original create.
                for (const auto& [mid, rs] : r.member_stamps) {
                    auto lit = local.member_stamps.find(mid);
                    if (lit == local.member_stamps.end()) {
                        local.member_stamps[mid] = rs;
                        local_changed = true;
                        continue;
                    }
                    const auto& ls = lit->second;
                    if (rs.logical_clock > ls.logical_clock
                        || (rs.logical_clock == ls.logical_clock
                            && !rs.is_member && ls.is_member)) {
                        lit->second = rs;
                        local_changed = true;
                    }
                }
                // Whole-record LWW for everything else (name,
                // settings, layout, input/clipboard caps,
                // tombstone). Membership stamps merged above
                // already; we don't want the whole-row swap to
                // clobber them with the remote's snapshot.
                if (r.version_ns > local.version_ns) {
                    auto saved_stamps = std::move(local.member_stamps);
                    local = std::move(r);
                    // Re-merge any stamps the remote might have
                    // missing — saved_stamps holds the post-merge
                    // value from above.
                    for (auto& [mid, st] : saved_stamps) {
                        auto& dst = local.member_stamps[mid];
                        if (st.logical_clock > dst.logical_clock
                            || (st.logical_clock == dst.logical_clock
                                && !st.is_member && dst.is_member)) {
                            dst = std::move(st);
                        }
                    }
                    local_changed = true;
                }
                if (local_changed) {
                    recompute_members_locked(local);
                    any_change = true;
                }
            }
            if (any_change) save_locked();
        }
        if (any_change) notify({});
    }

    void set_on_changed(OnChangedFn cb) override {
        std::lock_guard lk(m_);
        on_changed_ = std::move(cb);
    }

private:
    static std::unordered_set<std::string> union_of(
        const std::unordered_set<std::string>& a,
        const std::unordered_set<std::string>& b) {
        std::unordered_set<std::string> out = a;
        for (const auto& x : b) out.insert(x);
        return out;
    }

    /// @brief Rebuild @c ws.members from @c ws.member_stamps and
    /// auto-tombstone the workspace when fewer than 2 stamps have
    /// `is_member==true`. The tombstone is what propagates the
    /// "workspace gone" decision over the existing whole-row LWW;
    /// per-stamp updates ride the new member_stamps wire field.
    /// Tombstoning bumps `version_ns` so peers without the new
    /// fields still observe the deletion via their LWW path.
    void recompute_members_locked(Workspace& ws) {
        ws.members.clear();
        for (const auto& [mid, st] : ws.member_stamps) {
            if (st.is_member) ws.members.insert(mid);
        }
        ws.input_members     = clamp_to(ws.input_members,     ws.members);
        ws.clipboard_members = clamp_to(ws.clipboard_members, ws.members);
        if (!ws.tombstone && ws.members.size() < 2) {
            ws.tombstone = true;
            ws.version_ns = now_ns();
        }
    }

    /// @brief Clamp @p src to a subset of @p whitelist. Used to
    /// enforce input_members ⊆ members and clipboard_members ⊆
    /// members in every mutation path.
    static std::unordered_set<std::string> clamp_to(
        const std::unordered_set<std::string>& src,
        const std::unordered_set<std::string>& whitelist) {
        std::unordered_set<std::string> out;
        out.reserve(src.size());
        for (const auto& m : src) {
            if (whitelist.count(m) > 0) out.insert(m);
        }
        return out;
    }

    /// @brief Remove @p members from every workspace whose id is
    /// not @p except_id. Caller holds @c m_. Writes a fresh
    /// is_member=false stamp for each evicted PC so the eviction
    /// is visible to peers via the per-stamp LWW path; also bumps
    /// version_ns for the legacy whole-row LWW peers.
    void evict_members_locked(const std::unordered_set<std::string>& members,
                              const std::string& except_id) {
        const std::uint64_t clk = now_ns();
        for (auto& [wid, ws] : workspaces_) {
            if (wid == except_id || ws.tombstone) continue;
            bool changed = false;
            for (const auto& mid : members) {
                if (ws.members.count(mid) == 0) continue;
                auto& st = ws.member_stamps[mid];
                st = MemberStamp{false,
                                  std::max(clk, st.logical_clock + 1)};
                changed = true;
            }
            if (changed) {
                ws.version_ns = clk;
                recompute_members_locked(ws);
            }
        }
    }

    void notify(const std::string& id) {
        OnChangedFn cb;
        {
            std::lock_guard lk(m_);
            cb = on_changed_;
        }
        if (cb) cb(id);
    }

    /// @brief Caller holds @c m_.
    void save_locked() {
        const std::string path = catalogue_path();
        if (path.empty()) return;
        std::vector<Workspace> all;
        all.reserve(workspaces_.size());
        for (const auto& [_, ws] : workspaces_) all.push_back(ws);
        std::sort(all.begin(), all.end(),
                  [](const Workspace& a, const Workspace& b) {
                      return a.id < b.id;
                  });
        const std::string body = encode_workspaces_json(all);
        // Write atomically: write to .tmp then rename. Avoids a
        // half-written file if the process dies mid-write.
        const std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f.write(body.data(), static_cast<std::streamsize>(body.size()));
        }
        std::rename(tmp.c_str(), path.c_str());
    }

    void load_from_disk() {
        const std::string path = catalogue_path();
        if (path.empty()) return;
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string body = ss.str();
        std::vector<Workspace> parsed;
        if (!decode_workspaces_json(body, parsed)) return;
        std::lock_guard lk(m_);
        for (auto& ws : parsed) {
            // Older saved files (pre-cap-split) stored only
            // `members`. Promote those to "all caps on" so
            // existing workspaces don't silently lose features.
            // Legacy `keyboard_members` (from when cursor and
            // keyboard had separate per-peer toggles) gets
            // unioned into input_members on parse — see the
            // JSON parser above — so by this point input_members
            // already covers everything that was ever ticked.
            if (ws.input_members.empty() && ws.clipboard_members.empty()
                && !ws.members.empty()) {
                ws.input_members     = ws.members;
                ws.clipboard_members = ws.members;
            }
            // Some intermediate files were saved without the
            // top-level `members` field (a brief regression that
            // wiped membership on reload via the clamp below).
            // Defensively re-derive `members` from the union of
            // per-cap sets when the JSON omitted it.
            if (ws.members.empty()
                && (!ws.input_members.empty()
                    || !ws.clipboard_members.empty())) {
                for (const auto& m : ws.input_members)     ws.members.insert(m);
                for (const auto& m : ws.clipboard_members) ws.members.insert(m);
            }
            // Older saved files don't carry per-PC member_stamps.
            // Bootstrap them from the legacy `members` set with
            // clock=1 so any subsequent local write (clock = now_ns
            // ≫ 1) wins via LWW. clock=0 is reserved for "no real
            // stamp received yet" (synthesized when a peer omits
            // the field on the wire).
            if (ws.member_stamps.empty()) {
                for (const auto& mid : ws.members) {
                    ws.member_stamps[mid] = MemberStamp{true, 1};
                }
            }
            // recompute_members_locked clamps caps and rebuilds
            // the derived `members` set from stamps, plus auto-
            // tombstones if the projection drops below 2.
            recompute_members_locked(ws);
            workspaces_.emplace(ws.id, std::move(ws));
        }
    }

    mutable std::mutex                              m_;
    std::unordered_map<std::string, Workspace>     workspaces_;
    OnChangedFn                                     on_changed_;
};

}  // namespace

std::unique_ptr<IWorkspaceManager> make_workspace_manager() {
    return std::make_unique<WorkspaceManager>();
}

}  // namespace xorio::orchestrator
