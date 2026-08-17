#pragma once

#include "robot_adapter_interfaces/types.hpp"
#include "robot_switch_server/core/adapter_runtime_manager.hpp"

#include <string>
#include <vector>

namespace robot_switch_server {

// Builds JSON responses for HTTP API endpoints
class JsonResponseBuilder {
public:
    // Build response for /start and /stop operations
    static std::string BuildOperationResponse(
        const robot_adapter_interfaces::OperationResult& operation,
        const robot_adapter_interfaces::SwitchStatusSnapshot& snapshot);

    // Build response for /status endpoint
    static std::string BuildStatusResponse(
        const robot_adapter_interfaces::SwitchStatusSnapshot& snapshot);

    // Build response for /system_info endpoint
    static std::string BuildSystemInfoResponse(
        const AdapterRuntimeManager::SystemInfoResult& system_info,
        const robot_adapter_interfaces::SwitchStatusSnapshot& snapshot);

    // Build error response for bad requests
    static std::string BuildBadRequestResponse(const std::string& message);

    // Build simple health check response
    static std::string BuildHealthResponse();

    // Build response for /adapters endpoint
    static std::string BuildAdaptersResponse(const std::vector<std::string>& enabled_types);

    // Build response for /motion endpoint
    static std::string BuildMotionResponse(
        const AdapterRuntimeManager::InvokeMotionResult& result);

private:
    // Helper to safely get active adapter name
    static std::string GetActiveAdapterName(
        const robot_adapter_interfaces::SwitchStatusSnapshot& snapshot);
};

}  // namespace robot_switch_server
