/// @file path_selector.hpp
/// @brief Pure-function router that picks a codec path between
/// two peers' displays.
#pragma once

#include "orchestrator/mesh_crdt.hpp"
#include "orchestrator/stream.hpp"

#include <string>
#include <variant>
#include <vector>

namespace xorio_ui::orchestrator {

/// @brief One viable encode/decode/transport chain.
struct RoutingChoice {
    std::string codec;        ///< e.g. "h264".
    std::string encoder;      ///< e.g. "nvenc".
    std::string decoder;      ///< e.g. "vaapi".
    std::string presenter;    ///< e.g. "egl-x11".
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::uint32_t fps    = 0;
};

/// @brief Reason the selector could not produce a @ref RoutingChoice.
struct RoutingRefusal {
    enum class Reason {
        NoCommonCodec,
        SourceDisplayUnknown,
        SinkDisplayUnknown,
        ModeUnsupported,
        NoEncoderOnSource,
        NoDecoderOnSink,
        NoPresenterOnSink,
    };
    Reason      reason;
    std::string detail;
};

/// @brief Input the selector consults to make a routing decision.
struct RoutingInputs {
    std::string      src_machine_id;
    std::string      src_monitor_id;
    std::string      dst_machine_id;
    std::string      dst_monitor_id;
    RoutingMode      mode;
    const CapsRecord* src_caps;   ///< Nullable; @c nullptr = unknown peer.
    const CapsRecord* dst_caps;
};

/// @brief Pure-function routing oracle.
///
/// Implementations must have no hidden state, no network calls,
/// no global data. Every invocation with the same @ref
/// RoutingInputs returns the same result.
class IPathSelector {
public:
    virtual ~IPathSelector() = default;

    /// @brief Pick the best viable path between two endpoints.
    /// @return A @ref RoutingChoice on success, @ref RoutingRefusal
    /// with structured cause on failure.
    virtual std::variant<RoutingChoice, RoutingRefusal>
        pick(const RoutingInputs& in) const = 0;
};

}  // namespace xorio_ui::orchestrator
