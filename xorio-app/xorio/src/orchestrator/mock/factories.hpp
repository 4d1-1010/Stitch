/// @file factories.hpp
/// @brief Private factory declarations for the mock sub-modules.
/// Only the façade in @c orchestrator.cpp consumes these.
#pragma once

#include "orchestrator/control_connection.hpp"
#include "orchestrator/discovery.hpp"
#include "orchestrator/media_connection.hpp"
#include "orchestrator/mesh_crdt.hpp"
#include "orchestrator/pairing_manager.hpp"
#include "orchestrator/path_selector.hpp"
#include "orchestrator/session_scheduler.hpp"

#include <memory>
#include <string>

namespace xorio::orchestrator {

std::unique_ptr<IMeshCrdt>                 make_mock_mesh_crdt(std::string local_id);
std::unique_ptr<IPairingManager>           make_mock_pairing_manager();
std::unique_ptr<IControlConnectionManager> make_mock_control_connection_manager();
std::unique_ptr<IMediaConnectionFactory>   make_mock_media_connection_factory();
std::unique_ptr<IPathSelector>             make_mock_path_selector();
std::unique_ptr<ISessionScheduler>         make_mock_session_scheduler();

}  // namespace xorio::orchestrator
