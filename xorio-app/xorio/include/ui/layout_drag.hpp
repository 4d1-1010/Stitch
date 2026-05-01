/// @file layout_drag.hpp
/// @brief Drag-to-rearrange interaction for the Layout tab.
///
/// Scope: state struct + helpers for picking up a display rect,
/// tracking it under the mouse with AABB collision avoidance, and
/// committing the result on release. Pure interaction code — the
/// canvas drawing lives in `layout.cpp`. Both files together keep
/// each at < 350 LOC with one clear concern.
#pragma once

#include "imgui.h"

#include "orchestrator/display.hpp"

#include <chrono>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xorio::orchestrator { class IOrchestrator; }

namespace xorio::ui::layout {

/// @brief Stable identity key for one display rectangle.
using DisplayKey = std::pair<std::string, std::string>;

/// @brief Cross-frame drag state.
///
/// Overrides are stored in display-coordinate units (the same
/// space as `Display::global_x` / `global_y`) so the rect's
/// committed position is invariant under canvas pan / scale
/// changes — earlier we stored screen-pixel deltas, but the
/// auto-fit recomputed each frame based on `override / scale`,
/// and as smoothing converged the scale changed, so the global
/// delta drifted and the rect kept sliding after release.
/// `live_delta` is still in screen pixels (mouse-tracking),
/// converted to display units once on commit. `std::map` keeps
/// iteration order stable so the UI doesn't visually flicker
/// between frames.
struct DragState {
    bool                          active = false;
    DisplayKey                    target;
    ImVec2                        grab_offset{0, 0};
    ImVec2                        live_delta{0, 0};
    std::map<DisplayKey, ImVec2>  overrides;
    /// @brief Scale (display-px → screen-px) used in the most
    /// recent draw_displays pass. Apply reads this to translate
    /// screen-pixel drag deltas back into mesh-global coords.
    float                         last_scale = 1.0f;
    /// @brief Scale captured at the moment the drag started.
    /// `fit_displays` divides `live_delta` (screen pixels) by
    /// this to get a display-unit delta — using `last_scale`
    /// instead created a feedback loop where shrinking scale
    /// inflated the delta which shrank the scale further, so
    /// each cursor pixel zoomed the canvas out exponentially.
    /// With a stable divisor, strip extents grow linearly with
    /// cursor motion and the auto-fit re-frames at a sane rate.
    float                         scale_start = 1.0f;
};

/// @brief Lookup helper. Returns the saved override for @p key
/// or `(0, 0)` if none.
ImVec2 saved_offset(const DragState& drag, const DisplayKey& key);

/// @brief If the user just clicked inside the rectangle and no
/// drag is active, transition into a drag state anchored to the
/// click position.
void try_start_drag(const DisplayKey& key,
                    float sx, float sy, float sw, float sh,
                    ImVec2 mouse, DragState& drag);

/// @brief Update `drag.live_delta` from the mouse position with
/// AABB collision against every other display rectangle and
/// containment to the canvas. The rect cannot leave the canvas
/// even when the cursor goes off the app window — the cursor
/// just rides along the nearest canvas edge. If no non-
/// overlapping position fits inside the canvas, the rect
/// freezes at its last valid position instead of landing on
/// top of a neighbour.
///
/// `peer_render_offset` is the function `layout.cpp` uses to
/// shift each peer's coordinate space into a unique render column
/// — passed in so the collision check sees the same screen
/// positions the canvas does. `canvas_min` / `canvas_max` are
/// the screen-space corners the rect is clamped to.
///
/// `obstacles` is a list of additional rectangles the dragged
/// rect must not overlap but that are NOT rendered on the canvas
/// — used for offline workspace members' saved layout entries so
/// the user can't drop their own display on top of a reserved
/// position the offline peer will reclaim on reconnect. Their
/// `machine_id` may be absent from `peer_render_offset` (treated
/// as offset=0); their `global_x` / `global_y` are the saved
/// mesh-global coordinates from `Workspace::layout`.
void update_drag_with_collision(
    const std::vector<orchestrator::Display>& displays,
    const std::vector<orchestrator::Display>& obstacles,
    float scale, float pan_x, float pan_y,
    ImVec2 canvas_min, ImVec2 canvas_max,
    ImVec2 mouse,
    const std::map<std::string, std::int32_t>& peer_render_offset,
    DragState& drag);

/// @brief On mouse-up, fold @c drag.live_delta into the persisted
/// override map and clear the live state.
void commit_drag_release(DragState& drag);

/// @brief Snapshot of the current canvas geometry that Apply
/// needs to convert screen-pixel drag deltas back into mesh
/// display-pixel coordinates. Filled by @ref draw_displays each
/// frame and read by the Apply path.
struct ApplyContext {
    std::string                                    workspace_id;
    /// @brief Per-display effective base position used for
    /// rendering — already includes any active workspace layout
    /// override. Apply reads from this to compute final global
    /// positions after adding drag deltas (in screen px / scale).
    std::vector<orchestrator::Display>             displays;
    /// @brief render-time isotropic scale (display px → screen px).
    float                                          scale = 1.0f;
};

/// @brief Render the layout footer: Identify · Apply · Revert.
///
/// Identify fires the platform per-monitor fullscreen overlay
/// (see `platform/identify_overlay.hpp`); only the local PC's
/// monitors get overlays today — fanning the trigger to remote
/// peers needs the mesh control channel.
///
/// @param disabled  When true, every footer button renders greyed
///                  out and ignores clicks. Used by the Layout tab
///                  when no workspace is selectable.
/// @param ctx       Geometry snapshot Apply uses to compute new
///                  workspace.layout entries from the in-memory
///                  drag overrides.
void render_layout_footer(orchestrator::IOrchestrator& orch,
                          DragState& drag,
                          bool disabled,
                          const ApplyContext& ctx);

/// @brief Dwell duration for the Identify overlay. Matches the
/// Python tree's `IdentifyMsg.duration` default of 3 s.
inline constexpr std::chrono::seconds kIdentifyDwell{3};

}  // namespace xorio::ui::layout
