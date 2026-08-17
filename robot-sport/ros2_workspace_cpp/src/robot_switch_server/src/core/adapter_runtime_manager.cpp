#include "robot_switch_server/core/adapter_runtime_manager.hpp"
#include "robot_switch_server/utils/constants.hpp"
#include "robot_switch_server/utils/string_utils.hpp"

#include <ament_index_cpp/get_package_prefix.hpp>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace robot_switch_server {

namespace {

using robot_adapter_interfaces::ErrorCode;
using robot_adapter_interfaces::OperationResult;
using robot_adapter_interfaces::SwitchState;
using robot_adapter_interfaces::SwitchStatusSnapshot;

constexpr std::chrono::milliseconds kStartupProbeInterval{200};

template<typename... Args>
std::string BuildErrorDetail(Args&&... args) {
    std::string result;
    result.reserve(256);
    (result.append(std::forward<Args>(args)), ...);
    return result;
}

bool IsValidAdapterTypeChar(char ch) {
    const auto value = static_cast<unsigned char>(ch);
    return std::islower(value) || std::isdigit(value) || ch == '_';
}

OperationResult MakeError(ErrorCode code, std::string message, std::string detail) {
    return OperationResult{false, code, std::move(message), std::move(detail)};
}

OperationResult MakeSuccess(std::string message, std::string detail) {
    return OperationResult{true, ErrorCode::kNone, std::move(message), std::move(detail)};
}

std::unordered_map<std::string, robot_adapter_interfaces::MotionDescriptor>
ParseMotionsFromSystemInfo(const std::string& payload_json,
                           const rclcpp::Logger& logger) {
    std::unordered_map<std::string, robot_adapter_interfaces::MotionDescriptor>
        map;
    if (payload_json.empty()) {
        return map;
    }

    const auto parsed = nlohmann::json::parse(payload_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        RCLCPP_WARN(logger,
                    "motion cache: system_info payload not a JSON object; "
                    "motion set empty");
        return map;
    }

    const auto it = parsed.find("motions");
    if (it == parsed.end() || !it->is_array()) {
        return map;  // Adapter simply declared no motions; normal.
    }

    for (const auto& entry : *it) {
        if (!entry.is_object()) {
            RCLCPP_WARN(logger, "motion cache: entry not an object; dropped");
            continue;
        }
        robot_adapter_interfaces::MotionDescriptor desc;
        if (entry.contains("id") && entry.at("id").is_string()) {
            desc.id = entry.at("id").get<std::string>();
        }
        if (entry.contains("service_suffix") &&
            entry.at("service_suffix").is_string()) {
            desc.service_suffix =
                entry.at("service_suffix").get<std::string>();
        }
        if (entry.contains("description") &&
            entry.at("description").is_string()) {
            desc.description = entry.at("description").get<std::string>();
        }
        if (entry.contains("display_name") &&
            entry.at("display_name").is_string()) {
            desc.display_name = entry.at("display_name").get<std::string>();
        }

        if (!robot_adapter_interfaces::IsValidMotionId(desc.id)) {
            RCLCPP_WARN(logger,
                        "motion cache: invalid id '%s' dropped",
                        desc.id.c_str());
            continue;
        }
        if (desc.service_suffix.empty()) {
            RCLCPP_WARN(logger,
                        "motion cache: id '%s' has empty service_suffix; dropped",
                        desc.id.c_str());
            continue;
        }
        if (map.count(desc.id) != 0) {
            RCLCPP_WARN(logger,
                        "motion cache: duplicate id '%s' dropped",
                        desc.id.c_str());
            continue;
        }
        // 与 SystemInfoBuilder::SetMotions 保持同一条回落规则,兜住不用
        // SystemInfoBuilder、自己拼 /system_info JSON 的 adapter。
        if (desc.display_name.empty()) {
            desc.display_name = desc.id;
        }
        map.emplace(desc.id, std::move(desc));
    }
    return map;
}

}  // namespace

// static
robot_adapter_interfaces::SwitchState
AdapterRuntimeManager::MapToSwitchState(AdapterState state) {
    switch (state) {
    case AdapterState::kIdle:         return SwitchState::kDisconnected;
    case AdapterState::kStarting:     return SwitchState::kConnecting;
    case AdapterState::kRunning:      return SwitchState::kConnected;
    case AdapterState::kStopping:     return SwitchState::kDisconnecting;
    case AdapterState::kFaulted:      return SwitchState::kError;
    case AdapterState::kShuttingDown: return SwitchState::kDisconnected;
    }
    return SwitchState::kDisconnected;
}

AdapterRuntimeManager::AdapterRuntimeManager(
    rclcpp::Node::SharedPtr node,
    AdapterProcessSupervisor* supervisor,
    std::chrono::milliseconds service_wait,
    std::chrono::milliseconds call_timeout,
    std::chrono::milliseconds connect_timeout,
    const std::vector<std::string>& allowed_adapter_types)
    : node_(std::move(node))
    , supervisor_(supervisor)
    , service_wait_(service_wait)
    , call_timeout_(call_timeout)
    , connect_timeout_(connect_timeout) {
    for (const auto& raw_type : allowed_adapter_types) {
        if (auto normalized = NormalizeAdapterType(raw_type); !normalized.empty()) {
            allowed_adapter_types_.insert(std::move(normalized));
        }
    }
}

std::optional<OperationResult> AdapterRuntimeManager::ValidateStartConditions(
    const std::string& normalized_type) {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshCrashedProcessState();

    const auto current = state_machine_.state();

    // Reject if in a transient state (Starting, Stopping, ShuttingDown)
    if (current == AdapterState::kStarting || current == AdapterState::kStopping
        || current == AdapterState::kShuttingDown) {
        return MakeError(ErrorCode::kBusy,
                        "adapter transaction in progress",
                        BuildErrorDetail("state=", AdapterStateMachine::ToString(current)));
    }

    if (normalized_type.empty()) {
        return MakeError(ErrorCode::kUnknownRobot,
                        "invalid adapter_type",
                        "allowed pattern: [a-z0-9_]+");
    }

    if (!IsAllowedAdapterType(normalized_type)) {
        return MakeError(ErrorCode::kUnknownRobot,
                        "adapter_type is not enabled by config",
                        BuildErrorDetail("adapter_type=", normalized_type));
    }

    if (running_.has_value()) {
        return MakeError(ErrorCode::kPreconditionRequired,
                        "adapter instance already running; stop it first",
                        BuildErrorDetail("running=", running_->spec.adapter_type));
    }

    // Transition: Idle/Faulted → Starting
    if (!state_machine_.ProcessEvent(AdapterEvent::kStartRequested)) {
        return MakeError(ErrorCode::kBusy,
                        "state machine rejected start request",
                        BuildErrorDetail("state=", AdapterStateMachine::ToString(current)));
    }

    return std::nullopt;
}

std::pair<std::optional<AdapterRuntimeManager::RunningAdapter>, OperationResult>
AdapterRuntimeManager::TryLaunchAdapter(const std::string& normalized_type) {
    const auto spec = BuildAdapterSpec(normalized_type);
    if (!spec.has_value()) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_machine_.ProcessEvent(AdapterEvent::kStartFailed);
        return {std::nullopt, MakeError(ErrorCode::kUnknownRobot,
                                       "unable to build adapter spec from adapter_type",
                                       BuildErrorDetail("adapter_type=", normalized_type))};
    }

    std::string executable_path;
    std::string resolve_error;
    if (!ResolveExecutablePath(*spec, &executable_path, &resolve_error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_machine_.ProcessEvent(AdapterEvent::kStartFailed);
        return {std::nullopt, MakeError(ErrorCode::kUnknownRobot,
                                       "adapter executable not resolvable",
                                       std::move(resolve_error))};
    }

    const auto launch_res = supervisor_->Launch(executable_path);
    if (!launch_res.success) {
        std::lock_guard<std::mutex> lock(mutex_);
        state_machine_.ProcessEvent(AdapterEvent::kStartFailed);
        return {std::nullopt, MakeError(ErrorCode::kConnectFailed,
                                       "failed to start adapter process",
                                       launch_res.error_detail)};
    }

    RunningAdapter running;
    running.spec = *spec;
    running.executable_path = std::move(executable_path);
    running.pid = launch_res.pid;
    running.client = std::make_shared<robot_adapter_interfaces::AdapterClient>(
        node_, normalized_type, spec->service_prefix, service_wait_, call_timeout_);

    return {std::move(running), MakeSuccess("", "")};
}

OperationResult AdapterRuntimeManager::HandleConnectFailure(
    const RunningAdapter& running, const OperationResult& connect_result) {
    std::string stop_error;
    static_cast<void>(supervisor_->Stop(running.pid, &stop_error));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_machine_.ProcessEvent(AdapterEvent::kStartFailed);
    }

    if (stop_error.empty()) {
        return connect_result;
    }

    auto result = connect_result;
    if (!result.detail.empty()) {
        result.detail += "; ";
    }
    result.detail += BuildErrorDetail("stop_error=", stop_error);
    return result;
}

AdapterRuntimeManager::OperationWithSnapshot AdapterRuntimeManager::Start(
    const std::string& adapter_type) {
    const std::string normalized_type = NormalizeAdapterType(adapter_type);

    // Step 1: Validate and transition to Starting (under lock)
    if (const auto error = ValidateStartConditions(normalized_type)) {
        SetLastResult(*error);
        return {*error, GetStatusWithHealth()};
    }

    // Step 2: Launch adapter process (Starting state, no lock held)
    auto [running_opt, launch_result] = TryLaunchAdapter(normalized_type);
    if (!running_opt.has_value()) {
        SetLastResult(launch_result);
        return {launch_result, GetStatusWithHealth()};
    }

    // Step 3: Wait for adapter ready and connect (no lock held)
    const auto connect_result = WaitForAdapterReadyAndConnect(*running_opt);
    if (!connect_result.success) {
        auto result = HandleConnectFailure(*running_opt, connect_result);
        SetLastResult(result);
        return {result, GetStatusWithHealth()};
    }

    // Populate motion cache before committing state. Warn-and-continue on
    // failure — motion declaration is opt-in and non-critical to adapter health.
    {
        const auto si = running_opt->client->SystemInfo();
        UpdateMotionCacheFromSystemInfo(&*running_opt, si, "startup");
    }

    // Success: commit state under lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = std::move(*running_opt);
        state_machine_.ProcessEvent(AdapterEvent::kStartSucceeded);
    }

    auto result = MakeSuccess("adapter started and connected",
                             BuildErrorDetail("adapter_type=", normalized_type,
                                             "; pid=", std::to_string(running_->pid)));
    SetLastResult(result);
    return {result, GetStatusWithHealth()};
}

std::string AdapterRuntimeManager::CollectStopWarnings(
    robot_adapter_interfaces::AdapterClient& client) {
    std::string warning_detail;

    const auto safe_stop = client.SafeStop();
    if (!safe_stop.ok) {
        warning_detail += BuildErrorDetail("safe_stop=", safe_stop.message);
    }

    const auto disconnect = client.Disconnect();
    if (!disconnect.ok) {
        if (!warning_detail.empty()) {
            warning_detail += "; ";
        }
        warning_detail += BuildErrorDetail("disconnect=", disconnect.message);
    }

    return warning_detail;
}

void AdapterRuntimeManager::UpdateMotionCacheFromSystemInfo(
    RunningAdapter* running,
    const robot_adapter_interfaces::AdapterCallResult& system_info,
    const char* context) {
    if (running == nullptr) {
        return;
    }

    if (!system_info.reachable) {
        RCLCPP_WARN(
            node_->get_logger(),
            "motion cache: SystemInfo call failed during %s; motion set empty (%s)",
            context, system_info.message.c_str());
        return;
    }

    running->motions =
        ParseMotionsFromSystemInfo(system_info.message, node_->get_logger());

    if (!system_info.ok) {
        RCLCPP_WARN(
            node_->get_logger(),
            "motion cache: using degraded system_info payload during %s; cached %zu motion(s)",
            context, running->motions.size());
    }
}

void AdapterRuntimeManager::WaitForInflightMotionRpcsToDrain(
    std::unique_lock<std::mutex>* lock) {
    if (lock == nullptr || !lock->owns_lock()) {
        return;
    }

    motion_rpc_drained_cv_.wait(
        *lock, [this]() { return inflight_motion_rpcs_ == 0; });
}

void AdapterRuntimeManager::ReleaseInflightMotionRpc() {
    bool drained = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inflight_motion_rpcs_ == 0) {
            return;
        }
        --inflight_motion_rpcs_;
        drained = (inflight_motion_rpcs_ == 0);
    }

    if (drained) {
        motion_rpc_drained_cv_.notify_all();
    }
}

AdapterRuntimeManager::OperationWithSnapshot AdapterRuntimeManager::Stop() {
    // Step 1: Check state and transition under lock
    RunningAdapter running_copy;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        RefreshCrashedProcessState();

        const auto current = state_machine_.state();

        // Reject if in a transient state
        if (current == AdapterState::kStarting ||
            current == AdapterState::kStopping ||
            current == AdapterState::kShuttingDown) {
            auto err = MakeError(ErrorCode::kBusy,
                                "adapter transaction in progress",
                                BuildErrorDetail("state=", AdapterStateMachine::ToString(current)));
            SetLastResult(err);
            return {err, SnapshotStateLocked()};
        }

        if (!running_.has_value()) {
            auto result = MakeSuccess("no running adapter instance", "");
            SetLastResult(result);
            return {result, SnapshotStateLocked()};
        }

        // Transition: Running → Stopping
        if (!state_machine_.ProcessEvent(AdapterEvent::kStopRequested)) {
            auto err = MakeError(ErrorCode::kBusy,
                                "state machine rejected stop request",
                                BuildErrorDetail("state=", AdapterStateMachine::ToString(current)));
            SetLastResult(err);
            return {err, SnapshotStateLocked()};
        }

        running_copy = *running_;
        WaitForInflightMotionRpcsToDrain(&lock);
    }

    // Step 2: RPC calls outside the lock (may block)
    std::string warning_detail = CollectStopWarnings(*running_copy.client);

    // Step 3: Stop process and finalize state under lock
    std::string stop_error;
    const bool stopped = supervisor_->Stop(running_copy.pid, &stop_error);

    SwitchStatusSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.reset();
        state_machine_.ProcessEvent(AdapterEvent::kStopSucceeded);
        snapshot = SnapshotStateLocked();
    }

    if (!stopped) {
        auto result = MakeError(ErrorCode::kDisconnectFailed,
                               "failed to stop adapter process",
                               std::move(stop_error));
        SetLastResult(result);
        return {result, std::move(snapshot)};
    }

    std::string detail = warning_detail.empty()
        ? BuildErrorDetail("adapter_type=", running_copy.spec.adapter_type)
        : BuildErrorDetail("adapter_type=", running_copy.spec.adapter_type, "; ", warning_detail);

    auto result = MakeSuccess("adapter instance stopped", std::move(detail));
    SetLastResult(result);
    return {result, std::move(snapshot)};
}

SwitchStatusSnapshot AdapterRuntimeManager::SnapshotStateLocked() const {
    const auto current_state = state_machine_.state();
    SwitchStatusSnapshot snapshot;
    snapshot.state = MapToSwitchState(current_state);
    snapshot.active_robot = running_.has_value() ? running_->spec.adapter_type : "";
    snapshot.busy = (current_state == AdapterState::kStarting ||
                     current_state == AdapterState::kStopping);
    snapshot.last_code = last_code_;
    snapshot.last_message = last_message_;
    snapshot.last_detail = last_detail_;
    return snapshot;
}

SwitchStatusSnapshot AdapterRuntimeManager::GetStatusWithHealth() {
    // Step 1: take lock, refresh crash state, build snapshot, copy client out
    std::shared_ptr<robot_adapter_interfaces::AdapterClient> client_copy;
    std::string active_robot;
    SwitchStatusSnapshot snapshot;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        RefreshCrashedProcessState();
        snapshot = SnapshotStateLocked();
        if (running_.has_value()) {
            active_robot = running_->spec.adapter_type;
            client_copy = running_->client;
        }
    }

    // Step 2: call Health() outside the lock (may block on ROS2 service call)
    if (client_copy) {
        const auto health = client_copy->Health();
        robot_adapter_interfaces::AdapterStatus adapter_status;
        adapter_status.registered = true;
        adapter_status.reachable = health.reachable;
        adapter_status.available = health.ok;
        adapter_status.detail = health.message;
        snapshot.adapters[active_robot] = std::move(adapter_status);

        // Derive top-level state from health when state machine says Running
        if (snapshot.state == SwitchState::kConnected) {
            if (!health.reachable || !health.ok) {
                snapshot.state = SwitchState::kDisconnected;
                snapshot.last_code = ErrorCode::kTargetUnavailable;
                snapshot.last_message = health.reachable
                    ? "active adapter reports unhealthy"
                    : "adapter health service unreachable";
                snapshot.last_detail = health.message;
            }
        }
    }

    return snapshot;
}

AdapterRuntimeManager::SystemInfoResult AdapterRuntimeManager::GetSystemInfo() {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshCrashedProcessState();

    if (!running_.has_value()) {
        return SystemInfoResult{
            MakeError(ErrorCode::kPreconditionRequired,
                      "no running adapter instance",
                      "start an adapter first to query system_info"),
            "", ""};
    }

    const auto& spec = running_->spec;
    auto info = running_->client->SystemInfo();

    if (!info.reachable) {
        return SystemInfoResult{
            MakeError(ErrorCode::kTargetUnavailable,
                      "system_info service unreachable",
                      BuildErrorDetail("adapter_type=", spec.adapter_type,
                                      "; detail=", info.message)),
            spec.adapter_type, ""};
    }

    if (!info.ok) {
        return SystemInfoResult{
            MakeError(ErrorCode::kTargetUnavailable,
                      "system_info call failed",
                      BuildErrorDetail("adapter_type=", spec.adapter_type,
                                      "; detail=", info.message)),
            spec.adapter_type, ""};
    }

    return SystemInfoResult{
        MakeSuccess("system_info fetched",
                    BuildErrorDetail("adapter_type=", spec.adapter_type)),
        spec.adapter_type, std::move(info.message)};
}

std::string AdapterRuntimeManager::NormalizeAdapterType(const std::string& raw_type) const {
    std::string normalized = ToLowerCopy(TrimCopy(raw_type));

    if (normalized.empty()) {
        return "";
    }

    for (char ch : normalized) {
        if (!IsValidAdapterTypeChar(ch)) {
            return "";
        }
    }
    return normalized;
}

bool AdapterRuntimeManager::IsAllowedAdapterType(
    const std::string& adapter_type) const {
    if (allowed_adapter_types_.empty()) {
        return false;
    }
    return allowed_adapter_types_.count(adapter_type) != 0;
}

std::optional<AdapterRuntimeManager::AdapterSpec>
AdapterRuntimeManager::BuildAdapterSpec(const std::string& adapter_type) const {
    if (adapter_type.empty()) {
        return std::nullopt;
    }

    AdapterSpec spec;
    spec.adapter_type = adapter_type;
    spec.package_name = "adapter_" + adapter_type;
    spec.executable_name = spec.package_name + "_node";
    spec.service_prefix = "/" + spec.package_name;
    return spec;
}

bool AdapterRuntimeManager::ResolveExecutablePath(const AdapterSpec& spec,
                                                  std::string* path,
                                                  std::string* error_detail) const {
    if (!path || !error_detail) {
        return false;
    }

    try {
        const auto prefix = ament_index_cpp::get_package_prefix(spec.package_name);
        const auto executable_path = std::filesystem::path(prefix) / "lib" /
                                     spec.package_name / spec.executable_name;

        if (access(executable_path.c_str(), X_OK) != 0) {
            *error_detail = BuildErrorDetail("executable is not runnable: ",
                                            executable_path.string(),
                                            " (", std::strerror(errno), ")");
            return false;
        }

        *path = executable_path.string();
        return true;
    } catch (const std::exception& ex) {
        *error_detail = BuildErrorDetail("package lookup failed for '", spec.package_name,
                                        "': ", ex.what());
        return false;
    }
}

OperationResult AdapterRuntimeManager::WaitForAdapterReadyAndConnect(
    const RunningAdapter& running) {
    const auto deadline = std::chrono::steady_clock::now() + connect_timeout_;
    std::string last_probe_error = "adapter services not ready";

    while (std::chrono::steady_clock::now() < deadline) {
        if (!supervisor_->IsAlive(running.pid)) {
            return MakeError(ErrorCode::kConnectFailed,
                            "adapter process exited before connect succeeded",
                            BuildErrorDetail("adapter_type=", running.spec.adapter_type));
        }

        const auto health = running.client->Health();
        if (!health.reachable) {
            last_probe_error = BuildErrorDetail("health=", health.message);
            std::this_thread::sleep_for(kStartupProbeInterval);
            continue;
        }

        const auto connect = running.client->Connect();
        if (connect.ok) {
            return MakeSuccess("connected",
                              BuildErrorDetail("adapter_type=", running.spec.adapter_type,
                                              "; ", connect.message));
        }

        last_probe_error = BuildErrorDetail("connect=", connect.message);
        std::this_thread::sleep_for(kStartupProbeInterval);
    }

    return MakeError(ErrorCode::kConnectFailed,
                    "adapter connect timeout",
                    BuildErrorDetail("adapter_type=", running.spec.adapter_type,
                                    "; ", last_probe_error));
}

void AdapterRuntimeManager::SetLastResult(const OperationResult& result) {
    last_code_ = result.code;
    last_message_ = result.message;
    last_detail_ = result.detail;
}

void AdapterRuntimeManager::RefreshCrashedProcessState() {
    if (!running_.has_value()) {
        return;
    }

    // Check flag set by reaper callback
    if (child_exited_flag_ && child_exit_info_.pid == running_->pid) {
        state_machine_.ProcessEvent(AdapterEvent::kChildExitedUnexpectedly);
        last_code_ = ErrorCode::kDisconnectFailed;
        last_message_ = "adapter process exited unexpectedly";
        last_detail_ = BuildErrorDetail("adapter_type=", running_->spec.adapter_type,
                                        "; ", child_exit_info_.description);
        running_.reset();
        child_exited_flag_ = false;
        child_exit_info_ = {};
        return;
    }

    // Fallback: check if process is still alive
    if (!supervisor_->IsAlive(running_->pid)) {
        state_machine_.ProcessEvent(AdapterEvent::kChildExitedUnexpectedly);
        last_code_ = ErrorCode::kDisconnectFailed;
        last_message_ = "adapter process exited unexpectedly";
        last_detail_ = BuildErrorDetail("adapter_type=", running_->spec.adapter_type,
                                        "; child already reaped");
        running_.reset();
    }
}

void AdapterRuntimeManager::OnChildExited(const ProcessExitInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    child_exited_flag_ = true;
    child_exit_info_ = info;
}

std::vector<std::string> AdapterRuntimeManager::GetEnabledAdapterTypes() const {
    return std::vector<std::string>(allowed_adapter_types_.begin(), allowed_adapter_types_.end());
}

AdapterRuntimeManager::InvokeMotionResult AdapterRuntimeManager::InvokeMotion(
    const std::string& motion_id) {
    std::shared_ptr<robot_adapter_interfaces::AdapterClient> client;
    robot_adapter_interfaces::MotionDescriptor desc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RefreshCrashedProcessState();

        if (!running_.has_value()) {
            return {400, "no adapter running", "", motion_id};
        }
        if (state_machine_.state() != AdapterState::kRunning) {
            return {400,
                    "adapter not ready (state=" +
                        AdapterStateMachine::ToString(state_machine_.state()) +
                        ")",
                    "", motion_id};
        }
        if (!robot_adapter_interfaces::IsValidMotionId(motion_id)) {
            return {400, "invalid motion_id", "", motion_id};
        }
        const auto it = running_->motions.find(motion_id);
        if (it == running_->motions.end()) {
            return {400, "unknown motion_id '" + motion_id + "'", "",
                    motion_id};
        }
        client = running_->client;
        desc = it->second;
        ++inflight_motion_rpcs_;
    }

    // RPC outside the lock.
    robot_adapter_interfaces::AdapterCallResult rpc;
    try {
        rpc = client->CallTriggerByName(desc.service_suffix);
    } catch (const std::exception& ex) {
        ReleaseInflightMotionRpc();
        return {502, "adapter call failed", ex.what(), motion_id};
    } catch (...) {
        ReleaseInflightMotionRpc();
        return {502, "adapter call failed", "unknown motion RPC exception",
                motion_id};
    }
    ReleaseInflightMotionRpc();

    if (rpc.ok) {
        return {0, "success", rpc.message, motion_id};
    }
    if (rpc.failure ==
        robot_adapter_interfaces::AdapterCallFailure::kRejected) {
        return {502, "adapter rejected motion", rpc.message, motion_id};
    }
    return {502, "adapter call failed", rpc.message, motion_id};
}

}  // namespace robot_switch_server
