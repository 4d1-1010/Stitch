/// @file json_codec.hpp
/// @brief Internal JSON codec for the workspace catalogue.
///
/// Persists / restores the catalogue used by `manager.cpp`. We
/// deliberately don't pull in a JSON dependency — the shape is
/// fixed (root `{"workspaces":[...]}`) so a single-pass recursive
/// encoder + decoder is enough and keeps the third-party surface
/// at zero. Header is local to the workspace subsystem; nothing
/// outside `src/orchestrator/workspace/` should include it.
#pragma once

#include "orchestrator/workspace.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace xorio::orchestrator {

/// @brief Encode a workspace catalogue (including tombstones) as
/// a single JSON document.
std::string encode_workspaces_json(const std::vector<Workspace>& items);

/// @brief Decode a JSON document produced by
/// @ref encode_workspaces_json. Returns @c false on malformed
/// input; @p out is then in an indeterminate (partial) state.
bool decode_workspaces_json(std::string_view text,
                             std::vector<Workspace>& out);

}  // namespace xorio::orchestrator
