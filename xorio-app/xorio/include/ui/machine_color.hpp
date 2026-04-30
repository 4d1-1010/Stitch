/// @file machine_color.hpp
/// @brief Stable per-machine accent colour shared across screens.
///
/// Deterministic mapping `machine_id → ImVec4` so the same PC reads
/// the same hue everywhere: peer rows in Activity, PC nodes in
/// Layout, future per-PC chrome on stream tiles. Hue is biased
/// around the lilac brand accent so the palette stays cohesive.
///
/// Scope: pure function, no globals, no theme dependency beyond
/// `ImVec4`. The implementation in `src/ui/machine_color.cpp` owns
/// the CRC32 lookup table and HLS conversion as TU-local helpers.
#pragma once

#include "imgui.h"

#include <string>

namespace xorio::ui {

/// @brief Deterministic accent for @p machine_id.
ImVec4 machine_color(const std::string& machine_id);

}  // namespace xorio::ui
