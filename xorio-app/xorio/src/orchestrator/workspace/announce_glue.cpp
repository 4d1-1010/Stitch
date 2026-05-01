/// @file announce_glue.cpp
/// @brief Workspace ↔ announce wire-shape translation + the
/// idle auto-unlock worker tick.
///
/// Both functions are members of @ref FacadeOrchestrator (the
/// class lives in `orchestrator/facade.hpp`); they're split out
/// of `orchestrator.cpp` because they're pure shape-translation
/// + a periodic scan, not channel/dispatcher logic.

#include "orchestrator/facade.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xorio::orchestrator::detail {

std::vector<DisplayLayoutEntry>
FacadeOrchestrator::build_default_layout(
        const std::unordered_set<std::string>& members) const {
    std::vector<DisplayLayoutEntry> out;
    if (!mesh_) return out;
    // Same per-peer-column algorithm as the Layout tab's
    // compute_peer_render_offsets: order peers alphabetically
    // for stable output, give each its own horizontal column
    // sized to its own min..max extent, and separate columns
    // by kInterPeerGap so adjacency searches never confuse two
    // peers' edges. Sorted std::map so we walk in the same
    // order regardless of unordered_map's hash bucketing.
    constexpr std::int32_t kInterPeerGap = 200;
    struct PeerExtent { std::int32_t min_x = 0; std::int32_t max_x = 0; };
    std::map<std::string, PeerExtent> extents;
    std::map<std::string, std::vector<Display>> by_peer;
    for (const auto& [_, caps] : mesh_->all_caps()) {
        if (members.count(caps.machine_id) == 0) continue;
        for (const auto& d : caps.displays) {
            auto& list = by_peer[caps.machine_id];
            list.push_back(d);
            const std::int32_t lo = d.global_x;
            const std::int32_t hi = d.global_x + d.width;
            auto [it, inserted] =
                extents.emplace(caps.machine_id, PeerExtent{lo, hi});
            if (!inserted) {
                if (lo < it->second.min_x) it->second.min_x = lo;
                if (hi > it->second.max_x) it->second.max_x = hi;
            }
        }
    }
    if (extents.empty()) return out;
    std::int32_t cursor = 0;
    std::map<std::string, std::int32_t> offsets;
    for (const auto& [mid, ext] : extents) {
        offsets[mid] = cursor - ext.min_x;
        cursor += (ext.max_x - ext.min_x) + kInterPeerGap;
    }
    for (const auto& [mid, list] : by_peer) {
        const std::int32_t off = offsets.at(mid);
        for (const auto& d : list) {
            DisplayLayoutEntry e;
            e.machine_id = d.machine_id;
            e.monitor_id = d.monitor_id;
            e.global_x   = d.global_x + off;
            e.global_y   = d.global_y;
            out.push_back(std::move(e));
        }
    }
    return out;
}

std::vector<net::AnnounceWorkspace>
FacadeOrchestrator::wire_workspaces_for_announce() const {
    std::vector<net::AnnounceWorkspace> out;
    if (!workspaces_) return out;
    const auto state = workspaces_->wire_state();
    out.reserve(state.size());
    for (const auto& ws : state) {
        net::AnnounceWorkspace w;
        w.id         = ws.id;
        w.name       = ws.name;
        w.version_ns = ws.version_ns;
        w.tombstone  = ws.tombstone;
        auto fill = [](std::vector<std::string>& dst,
                       const std::unordered_set<std::string>& src) {
            dst.reserve(src.size());
            for (const auto& m : src) dst.push_back(m);
            std::sort(dst.begin(), dst.end());
        };
        fill(w.members,           ws.members);
        fill(w.input_members,     ws.input_members);
        fill(w.clipboard_members, ws.clipboard_members);
        w.clipboard_max          = static_cast<std::uint8_t>(ws.clipboard_max);
        w.clipboard_rich         = ws.clipboard_rich;
        w.clipboard_files        = ws.clipboard_files;
        w.cursor_edge_margin     = ws.cursor_edge_margin;
        w.cursor_require_modifier = ws.cursor_require_modifier;
        w.cursor_block_hotkeys   = ws.cursor_block_hotkeys;
        w.auto_unlock            = static_cast<std::uint8_t>(ws.auto_unlock);
        w.layout.reserve(ws.layout.size());
        for (const auto& e : ws.layout) {
            net::AnnounceWorkspace::LayoutEntry le;
            le.machine_id = e.machine_id;
            le.monitor_id = e.monitor_id;
            le.global_x   = e.global_x;
            le.global_y   = e.global_y;
            w.layout.push_back(std::move(le));
        }
        // Per-PC LWW stamps — sorted by machine_id for stable
        // wire output; the receiver merges by stamp regardless of
        // order, but a stable sort makes diffing announces easier.
        // Three parallel arrays: membership, input caps, clipboard
        // caps. Older receivers stop reading after the first one;
        // the cap blocks just disappear from the wire for them.
        auto fill_stamps = [](
                std::vector<net::AnnounceWorkspace::MemberStamp>& dst,
                const std::unordered_map<std::string, MemberStamp>& src) {
            std::vector<std::string> keys;
            keys.reserve(src.size());
            for (const auto& [mid, _] : src) keys.push_back(mid);
            std::sort(keys.begin(), keys.end());
            dst.reserve(keys.size());
            for (const auto& mid : keys) {
                const auto& st = src.at(mid);
                net::AnnounceWorkspace::MemberStamp m;
                m.machine_id    = mid;
                m.is_member     = st.is_member;
                m.logical_clock = st.logical_clock;
                dst.push_back(std::move(m));
            }
        };
        fill_stamps(w.member_stamps,            ws.member_stamps);
        fill_stamps(w.input_member_stamps,      ws.input_member_stamps);
        fill_stamps(w.clipboard_member_stamps,  ws.clipboard_member_stamps);
        w.locked                     = ws.locked;
        w.lock_unlock_after_h        = ws.lock_unlock_after_h;
        w.master_locked              = ws.master_locked;
        w.master_lock_unlock_after_h = ws.master_lock_unlock_after_h;
        w.master_locked_by           = ws.master_locked_by;
        out.push_back(std::move(w));
    }
    return out;
}

void FacadeOrchestrator::check_workspace_auto_unlock() {
    if (!workspaces_) return;
    using clk = std::chrono::system_clock;
    const std::uint64_t now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            clk::now().time_since_epoch()).count());
    constexpr std::uint64_t kHourNs = 3600ULL * 1000ULL * 1000ULL * 1000ULL;
    const auto items = workspaces_->list();
    for (const auto& ws : items) {
        const auto idle_ns = (now > ws.version_ns)
            ? (now - ws.version_ns) : std::uint64_t{0};
        if (ws.locked && ws.lock_unlock_after_h > 0
            && idle_ns > static_cast<std::uint64_t>(
                            ws.lock_unlock_after_h) * kHourNs) {
            // Clear regular Lock by re-setting the same settings
            // with `locked=false`. Other fields untouched.
            WorkspaceSettings s;
            s.clipboard_max          = ws.clipboard_max;
            s.clipboard_rich         = ws.clipboard_rich;
            s.clipboard_files        = ws.clipboard_files;
            s.cursor_edge_margin     = ws.cursor_edge_margin;
            s.cursor_require_modifier = ws.cursor_require_modifier;
            s.cursor_block_hotkeys   = ws.cursor_block_hotkeys;
            s.locked                 = false;
            s.lock_unlock_after_h    = ws.lock_unlock_after_h;
            workspaces_->set_settings(ws.id, s);
        }
        if (ws.master_locked && ws.master_lock_unlock_after_h > 0
            && idle_ns > static_cast<std::uint64_t>(
                            ws.master_lock_unlock_after_h) * kHourNs) {
            workspaces_->set_master_lock(
                ws.id, false, ws.master_lock_unlock_after_h,
                local_machine_id_);
        }
    }
}

}  // namespace xorio::orchestrator::detail
