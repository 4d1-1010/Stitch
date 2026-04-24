/// @file state.hpp
/// @brief Runtime licence-state enum observed by the UI.
#pragma once

namespace unio_ui::license {

/// @brief Coarse state the Access tab + tab-gating logic consult.
enum class LicenseState {
    Unactivated,    ///< No token installed.
    Activated,      ///< Signature valid, clock within @c [valid_from, expires_at].
    Expired,        ///< Wall-clock or counter crossed @c expires_at.
    TrialConsumed,  ///< Trial already used on this PC; no other token installed.
    Invalid,        ///< Signature failed / malformed / future-dated / tampered.
};

}  // namespace unio_ui::license
