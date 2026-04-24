/// @file pairing_manager.cpp
/// @brief In-memory pairing manager that treats every request
/// as auto-accepted (for mock-wiring demos).

#include "orchestrator/pairing_manager.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace unio_ui::orchestrator {

namespace {

class MockPairingManager final : public IPairingManager {
public:
    void set_callbacks(IncomingRequestFn on_incoming,
                       OutcomeFn         on_outcome) override {
        std::lock_guard lk(m_);
        on_incoming_ = std::move(on_incoming);
        on_outcome_  = std::move(on_outcome);
    }

    void request(const std::string& machine_id,
                 const std::string& /*invite_code*/) override {
        {
            std::lock_guard lk(m_);
            paired_[machine_id] = {crypto::PairingPublicKey{},
                                   machine_id, "", 0};
        }
        if (on_outcome_) {
            on_outcome_(machine_id, PairingOutcome::Accepted, "");
        }
    }

    void accept(const std::string& machine_id) override {
        std::lock_guard lk(m_);
        paired_[machine_id] = {crypto::PairingPublicKey{},
                               machine_id, "", 0};
    }

    void reject(const std::string& /*machine_id*/,
                const std::string& /*reason*/) override {}

    void unpair(const std::string& machine_id) override {
        std::lock_guard lk(m_);
        paired_.erase(machine_id);
    }

    std::vector<PairedPeer> list_paired() const override {
        std::lock_guard lk(m_);
        std::vector<PairedPeer> out;
        out.reserve(paired_.size());
        for (const auto& [_, v] : paired_) out.push_back(v);
        return out;
    }

    bool is_paired(const std::string& machine_id) const override {
        std::lock_guard lk(m_);
        return paired_.count(machine_id) != 0;
    }

private:
    mutable std::mutex                                   m_;
    std::unordered_map<std::string, PairedPeer>          paired_;
    IncomingRequestFn                                    on_incoming_;
    OutcomeFn                                            on_outcome_;
};

}  // namespace

std::unique_ptr<IPairingManager> make_mock_pairing_manager() {
    return std::make_unique<MockPairingManager>();
}

}  // namespace unio_ui::orchestrator
