/// @file control_connection.cpp
/// @brief In-memory @ref unio_ui::orchestrator::IControlConnection
/// and its manager. No real QUIC — calls are logged and state is
/// flipped synchronously.

#include "orchestrator/control_connection.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace unio_ui::orchestrator {

namespace {

class MockControlConnection final : public IControlConnection {
public:
    explicit MockControlConnection(std::string peer)
        : peer_(std::move(peer)) {}

    std::string peer_machine_id() const override { return peer_; }
    bool        is_established()  const override { return established_; }

    void publish(ControlStream /*slot*/,
                 const std::uint8_t* /*bytes*/,
                 std::size_t /*len*/) override {}

    void send_rpc(const RpcMessage& /*msg*/) override {}

    void set_handlers(
        std::function<void(Slot, const std::uint8_t*, std::size_t)> on_crdt,
        std::function<void(const RpcMessage&)> on_rpc) override {
        on_crdt_ = std::move(on_crdt);
        on_rpc_  = std::move(on_rpc);
    }

    void mark_established() { established_ = true; }

private:
    std::string                                                   peer_;
    bool                                                          established_ = false;
    std::function<void(Slot, const std::uint8_t*, std::size_t)>   on_crdt_;
    std::function<void(const RpcMessage&)>                        on_rpc_;
};

class MockControlConnectionManager final
    : public IControlConnectionManager {
public:
    void set_callbacks(OpenedFn on_opened, ClosedFn on_closed) override {
        std::lock_guard lk(m_);
        on_opened_ = std::move(on_opened);
        on_closed_ = std::move(on_closed);
    }

    void ensure_connection(const std::string& machine_id,
                           const std::string& /*address*/,
                           std::uint16_t /*port*/) override {
        std::unique_lock lk(m_);
        auto it = connections_.find(machine_id);
        if (it != connections_.end()) return;
        auto conn = std::make_unique<MockControlConnection>(machine_id);
        conn->mark_established();
        auto* raw = conn.get();
        connections_.emplace(machine_id, std::move(conn));
        auto cb = on_opened_;
        lk.unlock();
        if (cb) cb(*raw);
    }

    void close_connection(const std::string& machine_id) override {
        std::unique_lock lk(m_);
        connections_.erase(machine_id);
        auto cb = on_closed_;
        lk.unlock();
        if (cb) cb(machine_id);
    }

    std::vector<std::string> connected_peers() const override {
        std::lock_guard lk(m_);
        std::vector<std::string> out;
        out.reserve(connections_.size());
        for (const auto& [k, _] : connections_) out.push_back(k);
        return out;
    }

private:
    mutable std::mutex                                       m_;
    std::unordered_map<std::string,
                       std::unique_ptr<MockControlConnection>> connections_;
    OpenedFn                                                 on_opened_;
    ClosedFn                                                 on_closed_;
};

}  // namespace

std::unique_ptr<IControlConnectionManager>
make_mock_control_connection_manager() {
    return std::make_unique<MockControlConnectionManager>();
}

}  // namespace unio_ui::orchestrator
