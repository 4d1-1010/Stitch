/// @file workspace_manager.cpp
/// @brief In-memory mock @ref IWorkspaceManager.
///
/// Scope: catalogue + per-PC enforcement only. State is local to
/// this process; a future PR adds CRDT propagation across the
/// mesh. Concurrency is mutex-protected — the orchestrator may
/// call list() from the UI thread while a worker thread services
/// a mutation.

#include "orchestrator/workspace.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace unio_ui::orchestrator {

namespace {

/// @brief Generate a stable opaque id. Format `ws-NN` is enough for
/// the in-memory mock; the format intentionally avoids guarantees
/// the future CRDT impl will need to honour.
std::string generate_id() {
    static std::atomic<std::uint64_t> counter{0};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "ws-%llu",
                  static_cast<unsigned long long>(++counter));
    return buf;
}

class MockWorkspaceManager final : public IWorkspaceManager {
public:
    std::vector<Workspace> list() const override {
        std::lock_guard lk(m_);
        std::vector<Workspace> out;
        out.reserve(workspaces_.size());
        for (const auto& [_, ws] : workspaces_) out.push_back(ws);
        std::sort(out.begin(), out.end(),
                  [](const Workspace& a, const Workspace& b) {
                      if (a.name != b.name) return a.name < b.name;
                      return a.id < b.id;
                  });
        return out;
    }

    std::optional<Workspace> get(const std::string& id) const override {
        std::lock_guard lk(m_);
        auto it = workspaces_.find(id);
        if (it == workspaces_.end()) return std::nullopt;
        return it->second;
    }

    std::vector<std::pair<std::string, std::string>>
    pc_assignments() const override {
        std::lock_guard lk(m_);
        std::vector<std::pair<std::string, std::string>> out;
        out.reserve(workspaces_.size());
        for (const auto& [_, ws] : workspaces_) {
            for (const auto& mid : ws.members) {
                out.emplace_back(mid, ws.id);
            }
        }
        return out;
    }

    std::string create(const std::string& name,
                       const std::unordered_set<std::string>& members) override {
        std::string id = generate_id();
        {
            std::lock_guard lk(m_);
            evict_members_locked(members, /*except_id=*/{});
            Workspace ws;
            ws.id      = id;
            ws.name    = name;
            ws.members = members;
            workspaces_.emplace(id, std::move(ws));
        }
        notify(id);
        return id;
    }

    void rename(const std::string& id, const std::string& new_name) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it != workspaces_.end() && it->second.name != new_name) {
                it->second.name = new_name;
                changed = true;
            }
        }
        if (changed) notify(id);
    }

    void set_members(const std::string& id,
                     const std::unordered_set<std::string>& members) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it == workspaces_.end()) return;
            evict_members_locked(members, /*except_id=*/id);
            if (it->second.members != members) {
                it->second.members = members;
                changed = true;
            }
        }
        if (changed) notify(id);
    }

    void destroy(const std::string& id) override {
        bool removed = false;
        {
            std::lock_guard lk(m_);
            removed = workspaces_.erase(id) > 0;
        }
        if (removed) notify(id);
    }

    void acquire_lock(const std::string& id,
                      const std::string& machine_id) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it != workspaces_.end()
                && it->second.locked_by != machine_id) {
                it->second.locked_by = machine_id;
                changed = true;
            }
        }
        if (changed) notify(id);
    }

    void release_lock(const std::string& id,
                      const std::string& machine_id) override {
        bool changed = false;
        {
            std::lock_guard lk(m_);
            auto it = workspaces_.find(id);
            if (it != workspaces_.end()
                && it->second.locked_by == machine_id) {
                it->second.locked_by.clear();
                changed = true;
            }
        }
        if (changed) notify(id);
    }

    void set_on_changed(OnChangedFn cb) override {
        std::lock_guard lk(m_);
        on_changed_ = std::move(cb);
    }

private:
    /// @brief Remove @p members from every workspace whose id is
    /// not @p except_id. Caller holds @c m_.
    void evict_members_locked(const std::unordered_set<std::string>& members,
                              const std::string& except_id) {
        for (auto& [wid, ws] : workspaces_) {
            if (wid == except_id) continue;
            for (const auto& mid : members) {
                ws.members.erase(mid);
            }
        }
    }

    /// @brief Fire the change-notification callback outside the
    /// mutex so re-entrant queries from the UI thread don't
    /// deadlock against the mutation that triggered them.
    void notify(const std::string& id) {
        OnChangedFn cb;
        {
            std::lock_guard lk(m_);
            cb = on_changed_;
        }
        if (cb) cb(id);
    }

    mutable std::mutex                              m_;
    std::unordered_map<std::string, Workspace>     workspaces_;
    OnChangedFn                                     on_changed_;
};

}  // namespace

std::unique_ptr<IWorkspaceManager> make_mock_workspace_manager() {
    return std::make_unique<MockWorkspaceManager>();
}

}  // namespace unio_ui::orchestrator
