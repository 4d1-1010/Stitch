/// @file path_selector.cpp
/// @brief Trivial @ref unio_ui::orchestrator::IPathSelector that
/// always recommends h.264 @ 1920×1080 / 30 fps when both endpoints
/// have any encoder + decoder + presenter pair.

#include "orchestrator/path_selector.hpp"

#include <algorithm>
#include <memory>

namespace unio_ui::orchestrator {

namespace {

/// @brief @c true if @p v contains @p s. Retained for future
/// host-selection logic; reachable only when MockPathSelector
/// grows real preference rules.
[[maybe_unused]] bool contains(
        const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

class MockPathSelector final : public IPathSelector {
public:
    std::variant<RoutingChoice, RoutingRefusal>
    pick(const RoutingInputs& in) const override {
        if (!in.src_caps) {
            return RoutingRefusal{RoutingRefusal::Reason::SourceDisplayUnknown,
                                  in.src_machine_id};
        }
        if (!in.dst_caps) {
            return RoutingRefusal{RoutingRefusal::Reason::SinkDisplayUnknown,
                                  in.dst_machine_id};
        }
        if (in.src_caps->encoders.empty()) {
            return RoutingRefusal{RoutingRefusal::Reason::NoEncoderOnSource, {}};
        }
        if (in.dst_caps->decoders.empty()) {
            return RoutingRefusal{RoutingRefusal::Reason::NoDecoderOnSink, {}};
        }
        if (in.dst_caps->presenters.empty()) {
            return RoutingRefusal{RoutingRefusal::Reason::NoPresenterOnSink, {}};
        }

        RoutingChoice c;
        c.codec     = "h264";
        c.encoder   = in.src_caps->encoders.front();
        c.decoder   = in.dst_caps->decoders.front();
        c.presenter = in.dst_caps->presenters.front();
        c.width     = 1920;
        c.height    = 1080;
        c.fps       = 30;
        return c;
    }
};

}  // namespace

std::unique_ptr<IPathSelector> make_mock_path_selector() {
    return std::make_unique<MockPathSelector>();
}

}  // namespace unio_ui::orchestrator
