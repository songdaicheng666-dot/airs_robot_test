#include "robot_switch_server/http/json_response_builder.hpp"

#include "robot_switch_server/utils/constants.hpp"
#include "robot_switch_server/utils/json_utils.hpp"

namespace robot_switch_server {

namespace {

int64_t ToCode(robot_adapter_interfaces::ErrorCode code) {
    return static_cast<int64_t>(code);
}

std::string Envelope(int64_t code, const std::string& msg, const std::string& data_raw) {
    return JsonBuilder{512}
        .Add("code", code)
        .Add("msg", msg)
        .AddRaw("data", data_raw)
        .Build();
}

std::string EnvelopeNull(int64_t code, const std::string& msg) {
    return JsonBuilder{128}
        .Add("code", code)
        .Add("msg", msg)
        .AddNull("data")
        .Build();
}

}  // namespace

// static
std::string JsonResponseBuilder::GetActiveAdapterName(
    const robot_adapter_interfaces::SwitchStatusSnapshot& snapshot) {
    return snapshot.active_robot.empty() ? kUnknownAdapter : snapshot.active_robot;
}

std::string JsonResponseBuilder::BuildOperationResponse(
    const robot_adapter_interfaces::OperationResult& operation,
    const robot_adapter_interfaces::SwitchStatusSnapshot& snapshot) {
    const std::string data = JsonBuilder{256}
        .Add("active_adapter", GetActiveAdapterName(snapshot))
        .Add("state", robot_adapter_interfaces::ToString(snapshot.state))
        .Add("detail", operation.detail)
        .Build();
    return Envelope(ToCode(operation.code), operation.message, data);
}

std::string JsonResponseBuilder::BuildStatusResponse(
    const robot_adapter_interfaces::SwitchStatusSnapshot& snapshot) {
    JsonBuilder data_builder{512};
    data_builder.Add("state", robot_adapter_interfaces::ToString(snapshot.state))
                .Add("active_adapter", GetActiveAdapterName(snapshot))
                .Add("busy", snapshot.busy)
                .Add("last_code", robot_adapter_interfaces::ToString(snapshot.last_code))
                .Add("last_message", snapshot.last_message)
                .Add("last_detail", snapshot.last_detail);

    JsonArrayBuilder adapters_array;
    for (const auto& [robot_type, status] : snapshot.adapters) {
        JsonBuilder adapter_obj;
        adapter_obj.Add("robot_type", robot_type)
                   .Add("registered", status.registered)
                   .Add("reachable", status.reachable)
                   .Add("available", status.available)
                   .Add("detail", status.detail);
        adapters_array.AddRaw(adapter_obj.Build());
    }
    data_builder.AddRaw("adapters", adapters_array.Build());

    return Envelope(0, "success", data_builder.Build());
}

std::string JsonResponseBuilder::BuildSystemInfoResponse(
    const AdapterRuntimeManager::SystemInfoResult& system_info,
    const robot_adapter_interfaces::SwitchStatusSnapshot& snapshot) {
    JsonBuilder data_builder{512};
    data_builder.Add("active_adapter", GetActiveAdapterName(snapshot))
                .Add("state", robot_adapter_interfaces::ToString(snapshot.state));

    if (system_info.operation.success && LooksLikeJsonObject(system_info.payload)) {
        data_builder.AddRaw("system_info", system_info.payload);
    } else if (system_info.operation.success) {
        data_builder.Add("system_info", system_info.payload);
    } else {
        data_builder.AddNull("system_info");
    }

    return Envelope(ToCode(system_info.operation.code),
                    system_info.operation.message,
                    data_builder.Build());
}

std::string JsonResponseBuilder::BuildBadRequestResponse(const std::string& message) {
    return EnvelopeNull(400, message);
}

std::string JsonResponseBuilder::BuildHealthResponse() {
    return EnvelopeNull(0, "success");
}

std::string JsonResponseBuilder::BuildAdaptersResponse(
    const std::vector<std::string>& enabled_types) {
    JsonArrayBuilder arr;
    for (const auto& t : enabled_types) {
        arr.Add(t);
    }
    const std::string data = JsonBuilder{128}
        .AddRaw("enabled_adapter_types", arr.Build())
        .Build();
    return Envelope(0, "success", data);
}

std::string JsonResponseBuilder::BuildMotionResponse(
    const AdapterRuntimeManager::InvokeMotionResult& result) {
    if (result.code == 0 || result.code == 502) {
        const std::string data = JsonBuilder{256}
            .Add("motion_id", result.motion_id)
            .Add("detail", result.detail)
            .Build();
        return Envelope(static_cast<int64_t>(result.code), result.message,
                        data);
    }
    // 400 and anything else with no useful data payload.
    return EnvelopeNull(static_cast<int64_t>(result.code), result.message);
}

}  // namespace robot_switch_server
