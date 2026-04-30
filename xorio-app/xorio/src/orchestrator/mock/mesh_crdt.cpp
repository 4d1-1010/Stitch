/// @file mesh_crdt.cpp
/// @brief In-memory @ref xorio_ui::orchestrator::IMeshCrdt that
/// keeps the latest LWW record per @c (machine_id, slot) pair.

#include "orchestrator/mesh_crdt.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace xorio_ui::orchestrator {

namespace {

/// @brief Return steady_clock nanoseconds since epoch.
std::uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count());
}

class MockMeshCrdt final : public IMeshCrdt {
public:
    explicit MockMeshCrdt(std::string local_id)
        : local_id_(std::move(local_id)) {}

    void put_caps(const CapsRecord& rec) override {
        std::lock_guard lk(m_);
        caps_[rec.machine_id] = {rec, now_ns()};
    }
    void put_layout(const LayoutRecord& rec) override {
        std::lock_guard lk(m_);
        layout_ = {rec, now_ns()};
    }
    void put_workspaces(const WorkspacesRecord& rec) override {
        std::lock_guard lk(m_);
        workspaces_ = {rec, now_ns()};
    }
    void put_clipboard(const ClipboardRecord& rec) override {
        std::lock_guard lk(m_);
        clipboard_ = {rec, now_ns()};
    }
    void put_presence(const PresenceRecord& rec) override {
        std::lock_guard lk(m_);
        presence_[local_id_] = {rec, now_ns()};
    }

    std::optional<CapsRecord>
    caps_of(const std::string& machine_id) const override {
        std::lock_guard lk(m_);
        auto it = caps_.find(machine_id);
        if (it == caps_.end()) return std::nullopt;
        return it->second.value;
    }

    std::unordered_map<std::string, CapsRecord>
    all_caps() const override {
        std::lock_guard lk(m_);
        std::unordered_map<std::string, CapsRecord> out;
        out.reserve(caps_.size());
        for (const auto& [k, v] : caps_) out[k] = v.value;
        return out;
    }

    std::optional<LayoutRecord> current_layout() const override {
        std::lock_guard lk(m_);
        if (layout_.version == 0) return std::nullopt;
        return layout_.value;
    }

    std::vector<WorkspaceRecord> all_workspaces() const override {
        std::lock_guard lk(m_);
        return workspaces_.value.workspaces;
    }

    std::optional<ClipboardRecord> current_clipboard() const override {
        std::lock_guard lk(m_);
        if (clipboard_.version == 0) return std::nullopt;
        return clipboard_.value;
    }

    bool merge_signed_bytes(Slot /*slot*/,
                            const std::uint8_t* /*bytes*/,
                            std::size_t /*len*/) override {
        return true;
    }

    std::vector<std::uint8_t>
    serialize_slot(Slot /*slot*/) const override {
        return {};
    }

private:
    template <typename T>
    struct Versioned {
        T             value{};
        std::uint64_t version = 0;
    };

    mutable std::mutex m_;
    const std::string  local_id_;

    std::unordered_map<std::string, Versioned<CapsRecord>>     caps_;
    std::unordered_map<std::string, Versioned<PresenceRecord>> presence_;
    Versioned<LayoutRecord>     layout_;
    Versioned<WorkspacesRecord> workspaces_;
    Versioned<ClipboardRecord>  clipboard_;
};

}  // namespace

std::unique_ptr<IMeshCrdt> make_mock_mesh_crdt(std::string local_id) {
    return std::make_unique<MockMeshCrdt>(std::move(local_id));
}

}  // namespace xorio_ui::orchestrator
