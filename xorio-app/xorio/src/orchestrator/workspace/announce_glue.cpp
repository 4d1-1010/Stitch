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

void FacadeOrchestrator::ensure_workspace_layouts() {
    if (!workspaces_) return;
    const auto items = workspaces_->list();
    for (const auto& ws : items) {
        if (ws.tombstone) continue;
        if (ws.members.count(local_machine_id_) == 0) continue;
        if (!ws.layout.empty()) continue;
        if (auto_layout_attempted_.count(ws.id) > 0) continue;
        // Single-writer election: only the lexicographically
        // smallest member seeds. Without this, every member runs
        // build_default_layout in parallel; if any peer's view of
        // caps diverges (stale `mesh_->all_caps()` for a remote PC
        // mid-resync), each peer writes a slightly different layout
        // and LWW keeps whichever local set_layout had the latest
        // version_ns — so adi and Diana end up with mirror-imaged
        // mesh rects. Electing one writer makes the seed match
        // across peers; the loser receives the layout via announce.
        const std::string* smallest = &local_machine_id_;
        for (const auto& m : ws.members) {
            if (m < *smallest) smallest = &m;
        }
        if (*smallest != local_machine_id_) continue;
        // Need caps for every member before we can compute a
        // sensible default — otherwise we'd write a partial
        // layout and have to redo it later. Caps for the local
        // PC are always present once local_probe_ has run; remote
        // caps land via announce. Skipping when incomplete just
        // defers the auto-seed until the next announce arrives.
        bool all_caps_known = true;
        if (mesh_) {
            std::unordered_set<std::string> caps_for;
            for (const auto& [_, caps] : mesh_->all_caps()) {
                if (!caps.displays.empty()) caps_for.insert(caps.machine_id);
            }
            for (const auto& mid : ws.members) {
                if (caps_for.count(mid) == 0) {
                    all_caps_known = false;
                    break;
                }
            }
        }
        if (!all_caps_known) continue;
        auto layout = build_default_layout(ws.members);
        if (layout.empty()) continue;
        // Mark BEFORE the set_layout call so the on_changed
        // callback re-entering this function sees the workspace
        // as already-attempted and exits the loop. Without this
        // guard, set_layout → notify → on_changed → ensure...
        // would recurse before our caller's iteration finishes.
        auto_layout_attempted_.insert(ws.id);
        workspaces_->set_layout(ws.id, layout);
    }
}

void FacadeOrchestrator::apply_local_arrangement_if_needed() {
    if (!workspaces_ || !local_probe_) return;

    // Pick the active workspace using the same rule as
    // refresh_cursor_router_state: first non-tombstoned ws whose
    // member set contains us.
    const auto wss = workspaces_->list();
    const Workspace* active = nullptr;
    for (const auto& ws : wss) {
        if (ws.tombstone) continue;
        if (ws.members.count(local_machine_id_) == 0) continue;
        active = &ws;
        break;
    }
    if (active == nullptr) return;
    if (active->members.size() < 2) return;
    if (active->layout.empty()) return;

    // Filter layout entries to local-PC monitors and pull the
    // width/height for each from the local probe (the layout
    // entry only carries x/y; width/height live on the cap).
    auto current = local_probe_->probe();
    struct LocalEntry {
        std::string  monitor_id;
        std::int32_t mesh_x = 0;  ///< as the user laid it out
        std::int32_t mesh_y = 0;
        std::int32_t w      = 0;
        std::int32_t h      = 0;
    };
    std::vector<LocalEntry> mine;
    for (const auto& e : active->layout) {
        if (e.machine_id != local_machine_id_) continue;
        LocalEntry le;
        le.monitor_id = e.monitor_id;
        le.mesh_x     = e.global_x;
        le.mesh_y     = e.global_y;
        for (const auto& d : current.displays) {
            if (d.monitor_id == e.monitor_id) {
                le.w = d.width;
                le.h = d.height;
                break;
            }
        }
        if (le.w > 0 && le.h > 0) mine.push_back(std::move(le));
    }
    if (mine.empty()) return;

    // OS-local arrangements can't carry gaps between monitors —
    // X11 RandR and Win32 both treat the desktop as a single
    // contiguous region, and the cursor can't traverse an empty
    // band. The Layout tab is allowed to place a peer's monitor
    // between two of ours in mesh-global; we compact those gaps
    // away in OS-local while preserving the user's relative
    // ordering.
    //
    // Algorithm: group monitors into rows by y-overlap, sort
    // each row by mesh_x, pack edge-to-edge along x; stack rows
    // top-to-bottom with no vertical gap. Falls through to a
    // simple single-row pack when every monitor shares the
    // same horizontal band — the common multi-monitor case.
    std::sort(mine.begin(), mine.end(),
              [](const LocalEntry& a, const LocalEntry& b) {
                  if (a.mesh_y != b.mesh_y) return a.mesh_y < b.mesh_y;
                  return a.mesh_x < b.mesh_x;
              });

    struct Row {
        std::int32_t min_y     = 0;
        std::int32_t max_y     = 0;
        std::vector<std::size_t> idx;  ///< indices into mine[]
    };
    std::vector<Row> rows;
    for (std::size_t i = 0; i < mine.size(); ++i) {
        const auto& e = mine[i];
        const std::int32_t lo = e.mesh_y;
        const std::int32_t hi = e.mesh_y + e.h;
        bool placed = false;
        for (auto& r : rows) {
            if (lo < r.max_y && hi > r.min_y) {
                r.idx.push_back(i);
                if (lo < r.min_y) r.min_y = lo;
                if (hi > r.max_y) r.max_y = hi;
                placed = true;
                break;
            }
        }
        if (!placed) {
            rows.push_back(Row{lo, hi, {i}});
        }
    }
    // Within each row, sort by mesh_x so packing preserves the
    // user's left-to-right intent.
    for (auto& r : rows) {
        std::sort(r.idx.begin(), r.idx.end(),
                  [&](std::size_t a, std::size_t b) {
                      return mine[a].mesh_x < mine[b].mesh_x;
                  });
    }

    // Sort rows top-to-bottom for stable vertical stacking.
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) {
                  return a.min_y < b.min_y;
              });

    std::vector<DisplayPlacement> placements;
    placements.reserve(mine.size());
    std::int32_t row_y_offset = 0;
    for (const auto& r : rows) {
        std::int32_t cursor_x = 0;
        for (auto i : r.idx) {
            // Preserve each monitor's vertical offset within its
            // row — if the user dragged DP-1-0 down 300 px
            // relative to HDMI-1-0 in the Layout tab, the OS
            // should reflect that 300 px gap on this row's
            // baseline. Only the absolute mesh-y origin gets
            // collapsed (anchored to the row's top) and only the
            // x-axis gaps from peer monitors get packed away.
            DisplayPlacement p;
            p.monitor_id = mine[i].monitor_id;
            p.x          = cursor_x;
            p.y          = row_y_offset + (mine[i].mesh_y - r.min_y);
            placements.push_back(p);
            cursor_x += mine[i].w;
        }
        // Row height is the bounding-box span (max bottom edge
        // minus min top edge), so a row with vertically-offset
        // monitors still consumes the right vertical space and
        // the next row stacks below it without overlap.
        row_y_offset += (r.max_y - r.min_y);
    }

    // Skip the platform call when current OS arrangement already
    // matches desired — cheap idempotency that also avoids a
    // flicker when this fires from the periodic probe loop.
    bool match = true;
    for (const auto& p : placements) {
        bool found = false;
        for (const auto& d : current.displays) {
            if (d.monitor_id != p.monitor_id) continue;
            found = true;
            if (d.global_x != p.x || d.global_y != p.y) match = false;
            break;
        }
        if (!found) { match = false; break; }
    }
    if (match) return;
    local_probe_->apply_arrangement(placements);
}

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
