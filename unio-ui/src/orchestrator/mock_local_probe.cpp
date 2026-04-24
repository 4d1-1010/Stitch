/// @file mock_local_probe.cpp
/// @brief Fixed-data implementation of @ref unio_ui::orchestrator::ILocalProbeAdapter.

#include "orchestrator/local_probe.hpp"

#include <memory>

namespace unio_ui::orchestrator {

namespace {

class MockLocalProbe final : public ILocalProbeAdapter {
public:
    CapsRecord probe() const override {
        CapsRecord r;
        r.machine_id   = "adi-pc";
        r.display_name = "adi-pc (Linux)";
        r.displays = {
            {"adi-pc", "eDP-1",  0,    0, 1920, 1080, 1},
            {"adi-pc", "HDMI-1", 1920, 0, 2560, 1440, 2},
        };
        r.encoders         = {"nvenc-linux", "vaapi"};
        r.decoders         = {"nvdec", "vaapi"};
        r.presenters       = {"egl-x11"};
        r.capture_backends = {"xcomposite"};
        return r;
    }
};

}  // namespace

std::unique_ptr<ILocalProbeAdapter> make_mock_local_probe() {
    return std::make_unique<MockLocalProbe>();
}

}  // namespace unio_ui::orchestrator
