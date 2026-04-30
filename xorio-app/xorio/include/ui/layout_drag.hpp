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

namespace xorio_ui::orchestrator { class IOrchestrator; }

namespace xorio_ui::ui::layout {

/// @brief Stable identity key for one display rectangle.
using DisplayKey = std::pair<std::string, std::string>;

/// @brief Cross-frame drag state.
///
/// Overrides are stored in screen-space pixel deltas, applied on
/// top of the rectangle's natural pan-and-scaled position.
/// Persisting them through the orchestrator's mesh state is a
/// follow-up — for now Apply / Revert manipulate this in-memory
/// map only. `std::map` keeps iteration order stable so the UI
/// doesn't visually flicker between frames.
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
/// AABB collision against every other display rectangle. Iterated
/// up to 4 passes so a resolution that introduces a new overlap
/// gets resolved in the same frame; in densely-packed layouts
/// the rect just stops where it can't proceed.
///
/// `peer_render_offset` is the function `layout.cpp` uses to
/// shift each peer's coordinate space into a unique render column
/// — passed in so the collision check sees the same screen
/// positions the canvas does.
void update_drag_with_collision(
    const std::vector<orchestrator::Display>& displays,
    float scale, float pan_x, float pan_y,
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

}  // namespace xorio_ui::ui::layout
