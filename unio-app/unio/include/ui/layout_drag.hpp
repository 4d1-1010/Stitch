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

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace unio_ui::ui::layout {

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

/// @brief Render the Apply / Revert footer + dirty-state hint.
/// Apply currently just clears overrides — persistence through the
/// mesh CRDT is a follow-up.
void render_drag_footer(DragState& drag);

}  // namespace unio_ui::ui::layout
