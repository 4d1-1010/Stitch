/// @file tier.hpp
/// @brief Product tiers carried in a licence token.
///
/// Runtime enforcement is a follow-up; the field is captured on
/// the token today so the tool-chain + storage layout is stable
/// when gating is wired in.
#pragma once

#include <cstdint>

namespace xorio_ui::license {

/// @brief Product tier identifier.
enum class Tier : std::uint32_t {
    Trial      = 0,
    Home       = 1,
    Pro        = 2,
    Team       = 3,
    Enterprise = 4,
};

}  // namespace xorio_ui::license
