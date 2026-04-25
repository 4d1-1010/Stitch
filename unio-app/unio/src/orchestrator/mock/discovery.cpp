/// @file discovery.cpp
/// @brief Mock @ref unio_ui::orchestrator::IDiscovery that emits a
/// single simulated peer (Diana) after a short delay.

#include "orchestrator/discovery.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace unio_ui::orchestrator {

namespace {

class MockDiscovery final : public IDiscovery {
public:
    ~MockDiscovery() override { stop(); }

    void start(PeerObservedFn on_peer_observed,
               PeerLostFn     on_peer_lost) override {
        on_observed_ = std::move(on_peer_observed);
        on_lost_     = std::move(on_peer_lost);
        stop_flag_.store(false);
        worker_ = std::thread(&MockDiscovery::run, this);
    }

    void stop() override {
        stop_flag_.store(true);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void accept_manual_invite(const std::string& /*invite_code*/) override {}

private:
    void run() {
        std::unique_lock lk(m_);
        if (cv_.wait_for(lk, std::chrono::milliseconds(1000),
                         [&] { return stop_flag_.load(); })) {
            return;
        }

        DiscoveryAnnouncement a;
        a.machine_id   = "diana";
        a.display_name = "Diana (Windows)";
        a.address      = "192.168.1.18";
        a.control_port = 24900;
        for (std::size_t i = 0; i < a.public_key.bytes.size(); ++i) {
            a.public_key.bytes[i] = static_cast<std::uint8_t>(0xDAu ^ i);
        }

        if (on_observed_) on_observed_(a);
    }

    PeerObservedFn          on_observed_;
    PeerLostFn              on_lost_;
    std::thread             worker_;
    std::atomic<bool>       stop_flag_{false};
    std::mutex              m_;
    std::condition_variable cv_;
};

}  // namespace

std::unique_ptr<IDiscovery> make_mock_discovery() {
    return std::make_unique<MockDiscovery>();
}

}  // namespace unio_ui::orchestrator
