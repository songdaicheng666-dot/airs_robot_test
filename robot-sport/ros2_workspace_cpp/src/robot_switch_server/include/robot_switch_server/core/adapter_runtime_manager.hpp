#pragma once

#include "robot_adapter_interfaces/adapter_client.hpp"
#include "robot_adapter_interfaces/system_info.hpp"
#include "robot_adapter_interfaces/types.hpp"
#include "robot_switch_server/core/adapter_process_supervisor.hpp"
#include "robot_switch_server/core/adapter_state_machine.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/rclcpp.hpp>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace robot_switch_server {

class AdapterRuntimeManager {
public:
    struct SystemInfoResult {
        robot_adapter_interfaces::OperationResult operation;
        std::string adapter_type;
        std::string payload;
    };

    struct OperationWithSnapshot {
        robot_adapter_interfaces::OperationResult operation;
        robot_adapter_interfaces::SwitchStatusSnapshot snapshot;
    };

    struct InvokeMotionResult {
        int code{0};                // 0 success, 400 bad request, 502 adapter failure
        std::string message;
        std::string detail;
        std::string motion_id;
    };

    AdapterRuntimeManager(
        rclcpp::Node::SharedPtr node,
        AdapterProcessSupervisor* supervisor,
        std::chrono::milliseconds service_wait,
        std::chrono::milliseconds call_timeout,
        std::chrono::milliseconds connect_timeout,
        const std::vector<std::string>& allowed_adapter_types);

    [[nodiscard]] OperationWithSnapshot Start(const std::string& adapter_type);
    [[nodiscard]] OperationWithSnapshot Stop();
    [[nodiscard]] robot_adapter_interfaces::SwitchStatusSnapshot GetStatusWithHealth();
    [[nodiscard]] SystemInfoResult GetSystemInfo();
    [[nodiscard]] std::vector<std::string> GetEnabledAdapterTypes() const;
    [[nodiscard]] InvokeMotionResult InvokeMotion(const std::string& motion_id);

    void OnChildExited(const ProcessExitInfo& info);

private:
    struct AdapterSpec {
        std::string adapter_type;
        std::string package_name;
        std::string executable_name;
        std::string service_prefix;
    };

    struct RunningAdapter {
        AdapterSpec spec;
        std::string executable_path;
        pid_t pid{-1};
        std::shared_ptr<robot_adapter_interfaces::AdapterClient> client;
        std::unordered_map<std::string,
                           robot_adapter_interfaces::MotionDescriptor>
            motions;
    };

    [[nodiscard]] std::string NormalizeAdapterType(const std::string& raw_type) const;
    [[nodiscard]] bool IsAllowedAdapterType(const std::string& adapter_type) const;
    [[nodiscard]] std::optional<AdapterSpec> BuildAdapterSpec(const std::string& adapter_type) const;
    [[nodiscard]] bool ResolveExecutablePath(const AdapterSpec& spec, std::string* path,
                               std::string* error_detail) const;

    [[nodiscard]] robot_adapter_interfaces::OperationResult WaitForAdapterReadyAndConnect(
        const RunningAdapter& running);

    // Helper methods for Start() decomposition
    [[nodiscard]] std::optional<robot_adapter_interfaces::OperationResult> ValidateStartConditions(
        const std::string& normalized_type);
    [[nodiscard]] std::pair<std::optional<RunningAdapter>, robot_adapter_interfaces::OperationResult>
    TryLaunchAdapter(const std::string& normalized_type);
    [[nodiscard]] robot_adapter_interfaces::OperationResult HandleConnectFailure(
        const RunningAdapter& running, const robot_adapter_interfaces::OperationResult& connect_result);

    // Helper method for Stop()
    [[nodiscard]] std::string CollectStopWarnings(
        robot_adapter_interfaces::AdapterClient& client);
    void UpdateMotionCacheFromSystemInfo(
        RunningAdapter* running,
        const robot_adapter_interfaces::AdapterCallResult& system_info,
        const char* context);
    void WaitForInflightMotionRpcsToDrain(std::unique_lock<std::mutex>* lock);
    void ReleaseInflightMotionRpc();

    void SetLastResult(const robot_adapter_interfaces::OperationResult& result);
    void RefreshCrashedProcessState();

    // Builds a snapshot from current state (caller must hold mutex_).
    // Does NOT call Health() — use GetStatusWithHealth() for that.
    [[nodiscard]] robot_adapter_interfaces::SwitchStatusSnapshot SnapshotStateLocked() const;

    static robot_adapter_interfaces::SwitchState MapToSwitchState(AdapterState state);

    rclcpp::Node::SharedPtr node_;
    AdapterProcessSupervisor* supervisor_;
    std::chrono::milliseconds service_wait_;
    std::chrono::milliseconds call_timeout_;
    std::chrono::milliseconds connect_timeout_;
    std::set<std::string> allowed_adapter_types_;

    mutable std::mutex mutex_;
    std::condition_variable motion_rpc_drained_cv_;
    AdapterStateMachine state_machine_;
    std::optional<RunningAdapter> running_;
    std::size_t inflight_motion_rpcs_{0};
    robot_adapter_interfaces::ErrorCode last_code_{
        robot_adapter_interfaces::ErrorCode::kNone};
    std::string last_message_;
    std::string last_detail_;

    // Set by reaper callback, consumed by RefreshCrashedProcessState
    bool child_exited_flag_{false};
    ProcessExitInfo child_exit_info_;
};

}  // namespace robot_switch_server
