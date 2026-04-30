/// @file factory.hpp
/// @brief Private factory for the real @ref IWorkspaceManager
/// implementation. Only the façade in @c orchestrator.cpp consumes
/// this — the public surface is the interface in
/// `orchestrator/workspace.hpp`.
#pragma once

#include "orchestrator/workspace.hpp"

#include <memory>

namespace xorio_ui::orchestrator {

std::unique_ptr<IWorkspaceManager> make_workspace_manager();

}  // namespace xorio_ui::orchestrator
