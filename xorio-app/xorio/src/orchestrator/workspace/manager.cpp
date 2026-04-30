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
#include "orchestrator/workspace/json_codec.hpp"

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
                ws.input_member_stamps[mid] = MemberStamp{
                    input_members.count(mid) > 0, clk};
                ws.clipboard_member_stamps[mid] = MemberStamp{
                    clipboard_members.count(mid) > 0, clk};
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
                // otherwise). The same per-PC stamp shape applies
                // to the input + clipboard caps so two peers
                // adding different machines in parallel both keep
                // their tick choices through the merge instead of
                // the whole-row LWW dropping the slower writer's
                // selection.
                const std::uint64_t clk = now_ns();
                auto stamp_set =
                    [&clk](std::unordered_map<std::string, MemberStamp>& m,
                           const std::string& mid, bool on) {
                        auto& st = m[mid];
                        if (st.is_member != on || st.logical_clock < clk) {
                            st = MemberStamp{on, clk};
                        }
                    };
                for (const auto& mid : members) {
                    stamp_set(it->second.member_stamps, mid, true);
                    stamp_set(it->second.input_member_stamps,    mid,
                              in_clamped.count(mid) > 0);
                    stamp_set(it->second.clipboard_member_stamps, mid,
                              cb_clamped.count(mid) > 0);
                }
                for (const auto& mid : it->second.members) {
                    if (members.count(mid) == 0) {
                        stamp_set(it->second.member_stamps, mid, false);
                    }
                }
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
            st = MemberStamp{false, (std::max)(clk, st.logical_clock + 1)};
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
                || ws.locked              != s.locked
                || ws.lock_unlock_after_h != s.lock_unlock_after_h) {
                ws.clipboard_max           = s.clipboard_max;
                ws.clipboard_rich          = s.clipboard_rich;
                ws.clipboard_files         = s.clipboard_files;
                ws.cursor_edge_margin      = s.cursor_edge_margin;
                ws.cursor_require_modifier = s.cursor_require_modifier;
                ws.cursor_block_hotkeys    = s.cursor_block_hotkeys;
                ws.locked                  = s.locked;
                ws.lock_unlock_after_h     = s.lock_unlock_after_h;
                ws.version_ns              = now_ns();
                changed = true;
                save_locked();
            }
        }
        if (changed) notify(id);
    }

    void set_master_lock(const std::string& id,
                          bool enable,
                          std::uint32_t unlock_after_h,
                          const std::string& caller_machine_id) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it == workspaces_.end() || it->second.tombstone) return;
            auto& ws = it->second;
            // Compute master_locked_by transition: off→on records
            // the caller, on→off clears, on→on (no toggle) keeps
            // existing.
            std::string new_master_by = ws.master_locked_by;
            if (enable && !ws.master_locked) {
                new_master_by = caller_machine_id;
            } else if (!enable && ws.master_locked) {
                new_master_by.clear();
            }
            if (ws.master_locked              != enable
                || ws.master_lock_unlock_after_h != unlock_after_h
                || ws.master_locked_by        != new_master_by) {
                ws.master_locked              = enable;
                ws.master_lock_unlock_after_h = unlock_after_h;
                ws.master_locked_by           = std::move(new_master_by);
                ws.version_ns                 = now_ns();
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
                // Older peers don't send the per-PC stamp maps —
                // synthesize them from the legacy flat sets with
                // clock=0 so any locally-stamped value (clock>=1)
                // wins. Same shape for member / input / clipboard
                // so the merge above treats them uniformly.
                Workspace r = r_in;
                if (r.member_stamps.empty() && !r.members.empty()) {
                    for (const auto& mid : r.members) {
                        r.member_stamps[mid] = MemberStamp{true, 0};
                    }
                }
                if (r.input_member_stamps.empty()
                    && !r.input_members.empty()) {
                    for (const auto& mid : r.input_members) {
                        r.input_member_stamps[mid] = MemberStamp{true, 0};
                    }
                }
                if (r.clipboard_member_stamps.empty()
                    && !r.clipboard_members.empty()) {
                    for (const auto& mid : r.clipboard_members) {
                        r.clipboard_member_stamps[mid] = MemberStamp{true, 0};
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
                // Per-stamp LWW for membership and per-PC caps:
                // each machine's stamp merges by logical_clock;
                // ties bias toward is_member=false so a leave /
                // cap-off can't be undone by a stale "you're in"
                // stamp from an older snapshot.
                auto merge_stamp_map = [](
                        std::unordered_map<std::string, MemberStamp>& dst,
                        const std::unordered_map<std::string, MemberStamp>& src,
                        bool& any_change) {
                    for (const auto& [mid, rs] : src) {
                        auto lit = dst.find(mid);
                        if (lit == dst.end()) {
                            dst[mid] = rs;
                            any_change = true;
                            continue;
                        }
                        const auto& ls = lit->second;
                        if (rs.logical_clock > ls.logical_clock
                            || (rs.logical_clock == ls.logical_clock
                                && !rs.is_member && ls.is_member)) {
                            lit->second = rs;
                            any_change = true;
                        }
                    }
                };
                merge_stamp_map(local.member_stamps,
                                 r.member_stamps, local_changed);
                merge_stamp_map(local.input_member_stamps,
                                 r.input_member_stamps, local_changed);
                merge_stamp_map(local.clipboard_member_stamps,
                                 r.clipboard_member_stamps, local_changed);
                // Whole-record LWW for everything else (name,
                // settings, layout, input/clipboard caps,
                // tombstone). Membership stamps merged above
                // already; we don't want the whole-row swap to
                // clobber them with the remote's snapshot.
                if (r.version_ns > local.version_ns) {
                    auto saved_member_stamps = std::move(local.member_stamps);
                    auto saved_input_stamps  = std::move(local.input_member_stamps);
                    auto saved_clip_stamps   = std::move(local.clipboard_member_stamps);
                    local = std::move(r);
                    // Re-merge the stamps we already had post-
                    // merge above so the whole-row swap doesn't
                    // clobber our newer per-PC writes.
                    bool _ = false;
                    merge_stamp_map(local.member_stamps,
                                     saved_member_stamps, _);
                    merge_stamp_map(local.input_member_stamps,
                                     saved_input_stamps, _);
                    merge_stamp_map(local.clipboard_member_stamps,
                                     saved_clip_stamps, _);
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

    /// @brief Rebuild @c ws.members / input_members / clipboard_
    /// members from the per-PC stamp maps and auto-tombstone the
    /// workspace when fewer than 2 stamps have `is_member==true`.
    /// The tombstone is what propagates the "workspace gone"
    /// decision over the existing whole-row LWW; per-stamp updates
    /// ride the new *_stamps wire fields. Tombstoning bumps
    /// `version_ns` so peers without the new fields still observe
    /// the deletion via their LWW path.
    void recompute_members_locked(Workspace& ws) {
        ws.members.clear();
        for (const auto& [mid, st] : ws.member_stamps) {
            if (st.is_member) ws.members.insert(mid);
        }
        ws.input_members.clear();
        for (const auto& [mid, st] : ws.input_member_stamps) {
            if (st.is_member && ws.members.count(mid) > 0) {
                ws.input_members.insert(mid);
            }
        }
        ws.clipboard_members.clear();
        for (const auto& [mid, st] : ws.clipboard_member_stamps) {
            if (st.is_member && ws.members.count(mid) > 0) {
                ws.clipboard_members.insert(mid);
            }
        }
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
                                  (std::max)(clk, st.logical_clock + 1)};
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
            // Older saved files don't carry per-PC stamp maps.
            // Bootstrap them from the legacy flat sets with
            // clock=1 so any subsequent local write (clock = now_ns
            // ≫ 1) wins via LWW. clock=0 is reserved for "no real
            // stamp received yet" (synthesized when a peer omits
            // the field on the wire).
            if (ws.member_stamps.empty()) {
                for (const auto& mid : ws.members) {
                    ws.member_stamps[mid] = MemberStamp{true, 1};
                }
            }
            if (ws.input_member_stamps.empty()) {
                for (const auto& mid : ws.input_members) {
                    ws.input_member_stamps[mid] = MemberStamp{true, 1};
                }
            }
            if (ws.clipboard_member_stamps.empty()) {
                for (const auto& mid : ws.clipboard_members) {
                    ws.clipboard_member_stamps[mid] = MemberStamp{true, 1};
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
