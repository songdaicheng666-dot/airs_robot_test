#include "adapter_lynx/lynx_adapter_node.hpp"

#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>
#include <robot_adapter_interfaces/system_info.hpp>

namespace adapter_lynx {

// ---------------------------------------------------------------------------
// 构造：读 YAML 参数（框架的 GetParamOrDefault 从 adapter_lynx.yaml 读）
// ---------------------------------------------------------------------------

LynxAdapterNode::LynxAdapterNode() : AdapterNodeBase("lynx") {
    robot_ip_               = GetParamOrDefault<std::string>("robot_ip", "10.21.31.103");
    robot_port_             = GetParamOrDefault<int>("robot_port", 30000);
    local_port_             = GetParamOrDefault<int>("local_port", 0);
    heartbeat_interval_sec_ = GetParamOrDefault<double>("heartbeat_interval_sec", 1.0);
    recv_timeout_sec_       = GetParamOrDefault<double>("recv_timeout_sec", 1.0);
    LynxVelocityConversionConfig velocity_config;
    velocity_config.max_linear_x_mps =
        GetParamOrDefault<double>("max_linear_x", 1.5);
    velocity_config.max_linear_y_mps =
        GetParamOrDefault<double>("max_linear_y", 1.0);
    velocity_config.max_angular_z_radps =
        GetParamOrDefault<double>("max_angular_z", 2.0);
    velocity_config.full_scale_linear_x_mps =
        GetParamOrDefault<double>("lynx_full_scale_linear_x_mps", 2.0);
    velocity_config.full_scale_linear_y_mps =
        GetParamOrDefault<double>("lynx_full_scale_linear_y_mps", 2.0);
    velocity_config.full_scale_angular_z_radps =
        GetParamOrDefault<double>("lynx_full_scale_angular_z_radps", 2.0);
    velocity_converter_ = LynxVelocityConverter(velocity_config);
    initial_mode_ = GetParamOrDefault<int>(
        "initial_mode", LynxSdkClient::kUsageModeRegular);
    initial_motion_state_ = GetParamOrDefault<int>(
        "initial_motion_state", LynxSdkClient::kMotionStateStanding);
    cmd_vel_timeout_ms_     = GetParamOrDefault<int>("cmd_vel_timeout_ms", 500);
    watchdog_check_interval_ms_ = GetParamOrDefault<int>("watchdog_check_interval_ms", 100);
    query_timeout_ms_       = GetParamOrDefault<int>("query_timeout_ms", 1000);
    control_transition_timeout_ms_ =
        GetParamOrDefault<int>("control_transition_timeout_ms", 1500);
    status_stale_timeout_ms_ = GetParamOrDefault<int>("status_stale_timeout_ms", 2000);
    connect_status_timeout_ms_ =
        GetParamOrDefault<int>("connect_status_timeout_ms", status_stale_timeout_ms_);

    if (initial_mode_ != LynxSdkClient::kUsageModeRegular &&
        initial_mode_ != LynxSdkClient::kUsageModeNavigation &&
        initial_mode_ != LynxSdkClient::kUsageModeAssisted) {
        throw std::invalid_argument("initial_mode must be 0, 1, or 2");
    }
    if (control_transition_timeout_ms_ <= 0) {
        throw std::invalid_argument("control_transition_timeout_ms must be positive");
    }

    RCLCPP_INFO(get_logger(), "adapter_lynx ready → %s:%d",
                robot_ip_.c_str(), robot_port_);
    RCLCPP_INFO(
        get_logger(),
        "cmd_vel SI limits/full-scale: x=%.3f/%.3f m/s, y=%.3f/%.3f m/s, "
        "yaw=%.3f/%.3f rad/s",
        velocity_config.max_linear_x_mps, velocity_config.full_scale_linear_x_mps,
        velocity_config.max_linear_y_mps, velocity_config.full_scale_linear_y_mps,
        velocity_config.max_angular_z_radps,
        velocity_config.full_scale_angular_z_radps);
}

// ===========================================================================
// 框架必须实现的 5 个接口
// ===========================================================================

// ---------------------------------------------------------------------------
// OnConnect：建立 UDP 通信，启动心跳，切初始模式
// ---------------------------------------------------------------------------

void LynxAdapterNode::OnConnect(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(node_state_mutex_);

    if (connected_) {
        response->success = true;
        response->message = "Already connected";
        return;
    }

    // Clear stale status before reconnect attempt
    sdk_.ResetStatus();

    LynxConfig config;
    config.robot_ip               = robot_ip_;
    config.robot_port             = static_cast<uint16_t>(robot_port_);
    config.local_port             = static_cast<uint16_t>(local_port_);
    config.heartbeat_interval_sec = heartbeat_interval_sec_;
    config.recv_timeout_sec       = recv_timeout_sec_;

    std::string error;
    if (!sdk_.Initialize(config, &error)) {
        last_error_ = error;
        response->success = false;
        response->message = "SDK init failed: " + error;
        RCLCPP_ERROR(get_logger(), "Connect failed: %s", error.c_str());
        return;
    }

    // 默认保持旧行为（常规模式 + 站立），同时允许部署通过 YAML
    // 选择其他初始模式。初始零速必须使用与目标模式匹配的指令。
    const bool initial_commands_sent =
        sdk_.SetMode(initial_mode_) &&
        sdk_.SetMotionState(initial_motion_state_) &&
        SendZeroVelocityForMode(initial_mode_);
    if (!initial_commands_sent) {
        sdk_.Shutdown();
        last_error_ = "Connect failed: could not send initial control state";
        response->success = false;
        response->message = last_error_;
        RCLCPP_ERROR(get_logger(), "%s", last_error_.c_str());
        return;
    }

    // Consider connect successful only after at least one valid status frame
    // arrives from the robot. Otherwise a local UDP socket can start cleanly
    // even when the robot is unreachable on the current network segment.
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(connect_status_timeout_ms_);
    bool received_valid_status = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = sdk_.GetLatestStatus();
        if (status.valid) {
            received_valid_status = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!received_valid_status) {
        sdk_.Shutdown();
        last_error_ = "Connect failed: no valid status received within " +
            std::to_string(connect_status_timeout_ms_) + "ms";
        response->success = false;
        response->message = last_error_;
        RCLCPP_ERROR(get_logger(), "%s", last_error_.c_str());
        return;
    }

    connected_ = true;
    last_cmd_vel_time_ = {};
    last_error_.clear();
    response->success = true;
    response->message = "Connected to lynx @ " + robot_ip_;
    RCLCPP_INFO(get_logger(), "Connected → %s:%d", robot_ip_.c_str(), robot_port_);
}

// ---------------------------------------------------------------------------
// OnDisconnect：零速 → 停线程 → 关 socket
// ---------------------------------------------------------------------------

void LynxAdapterNode::OnDisconnect(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(node_state_mutex_);

    if (!connected_) {
        response->success = true;
        response->message = "Already disconnected";
        return;
    }

    {
        std::lock_guard<std::mutex> control_lock(control_mutex_);
        SendZeroVelocityForCurrentMode();
        last_cmd_vel_time_ = {};
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sdk_.Shutdown();

    connected_ = false;
    response->success = true;
    response->message = "Disconnected";
    RCLCPP_INFO(get_logger(), "Disconnected from lynx");
}

// ---------------------------------------------------------------------------
// OnSafeStop：立即发零速
// ---------------------------------------------------------------------------

void LynxAdapterNode::OnSafeStop(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(node_state_mutex_);

    if (!connected_) {
        response->success = true;
        response->message = "Not connected, nothing to stop";
        return;
    }

    std::lock_guard<std::mutex> control_lock(control_mutex_);
    const bool sent = SendZeroVelocityForCurrentMode();
    last_cmd_vel_time_ = {};
    response->success = sent;
    response->message = sent
        ? "safe_stop: zero velocity sent"
        : "safe_stop: failed to send zero velocity";
    RCLCPP_INFO(get_logger(), "Safe stop");
}

// ---------------------------------------------------------------------------
// OnHealth：返回关键状态快照 JSON
// ---------------------------------------------------------------------------

void LynxAdapterNode::OnHealth(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(node_state_mutex_);

    auto status = sdk_.GetLatestStatus();
    nlohmann::json j;
    j["connected"] = connected_;
    j["valid"]     = status.valid;

    if (status.valid) {
        const auto stale_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - status.last_update).count();
        j["stale_ms"]     = stale_ms;
        j["battery_pct"]  = status.battery_percentage;
        j["motion_state"] = status.motion_state;
        j["e_stop"]       = status.e_stop;
        j["charging"]     = status.charging;
        j["error_code"]   = status.error_code;

        const bool fresh = stale_ms <= status_stale_timeout_ms_;
        j["fresh"] = fresh;
        response->success = connected_ && fresh;
    } else {
        response->success = false;
    }

    if (!last_error_.empty()) j["last_error"] = last_error_;
    response->message = j.dump();
}

// ---------------------------------------------------------------------------
// OnSystemInfo：返回完整设备快照 JSON
// ---------------------------------------------------------------------------

void LynxAdapterNode::OnSystemInfo(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(node_state_mutex_);

    auto s = sdk_.GetLatestStatus();

    const bool stale = !s.valid ||
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - s.last_update).count() > status_stale_timeout_ms_;

    nlohmann::json j;
    j["adapter"]            = "lynx";
    j["connected"]          = connected_;
    j["valid"]              = s.valid;
    j["stale"]              = stale;
    j["motion_state"]       = s.motion_state;
    j["gait"]               = s.gait;
    j["control_usage_mode"] = s.control_usage_mode;
    j["sleeping"]           = s.sleeping;
    j["charging"]           = s.charging;
    j["e_stop"]             = s.e_stop;
    j["error"]["code"]      = s.error_code;
    j["error"]["component"] = s.error_component;
    j["battery"]["voltage"]    = s.battery_voltage;
    j["battery"]["current"]    = s.battery_current;
    j["battery"]["percentage"] = s.battery_percentage;
    j["velocity"]["x"]         = s.vel_x;
    j["velocity"]["y"]         = s.vel_y;
    j["velocity"]["yaw"]       = s.yaw_rate;
    j["attitude"]["pitch"]     = s.pitch;
    j["attitude"]["roll"]      = s.roll;
    j["attitude"]["yaw"]       = s.yaw;
    j["body_height"]           = s.body_height;
    if (!last_error_.empty()) j["last_error"] = last_error_;

    robot_adapter_interfaces::SystemInfoBuilder system_info;
    if (connected_ && !stale && s.battery_percentage >= 0) {
        system_info.SetBattery(s.battery_percentage);
    }
    if (connected_ && !stale) {
        system_info.SetMotion(s.vel_x, s.vel_y, s.yaw_rate);
    }
    system_info.SetMotions({
        {"mode_regular", "mode/regular",
         "Use normalized Command=21 axis control", "常规模式"},
        {"mode_navigation", "mode/navigation",
         "Use absolute Command=25 velocity control", "导航模式"},
        {"stand_up", "stand_up", "Switch to standing posture", "站立"},
        {"soft_stop", "soft_stop", "Switch to soft stop posture", "软急停"},
        {"sit_down", "sit_down", "Switch to prone posture", "趴下"},
        {"rl_control", "rl_control", "Enter RL control state", "RL 控制"},
        {"gait_standard_flat", "gait/standard_flat",
         "Prepare RL and switch to standard flat gait", "标准平地"},
        {"gait_standard_stairs", "gait/standard_stairs",
         "Prepare RL and switch to standard stairs gait", "标准爬楼"},
        {"gait_agile_flat", "gait/agile_flat",
         "Prepare usage mode/RL and switch to agile flat gait", "敏捷平地"},
        {"gait_agile_stairs", "gait/agile_stairs",
         "Prepare usage mode/RL and switch to agile stairs gait", "敏捷爬楼"},
    });
    system_info.SetDetailsJson(j.dump());

    response->success = connected_ && !stale;
    response->message = system_info.Build();
}

// ===========================================================================
// RegisterExtensions：注册全部扩展 Service 和 Topic
// ===========================================================================

void LynxAdapterNode::RegisterExtensions() {
    const std::string p = "/" + std::string(get_name()) + "/";

    // ---------- 运动控制 Topic ----------
    cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
        GetCmdVelTopic(), 10,
        [this](const geometry_msgs::msg::Twist::SharedPtr m) { OnCmdVel(m); });

    // ---------- 看门狗定时器 ----------
    watchdog_timer_ = create_wall_timer(
        std::chrono::milliseconds(watchdog_check_interval_ms_),
        [this]() { OnWatchdogTick(); });

    using Req = std::shared_ptr<std_srvs::srv::Trigger::Request>;

    // ---------- 模式切换 ----------
    mode_regular_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "mode/regular",
        [this](Req, TriggerResponse res) { OnModeRegular(res); });
    mode_navigation_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "mode/navigation",
        [this](Req, TriggerResponse res) { OnModeNavigation(res); });

    // ---------- 步态切换 ----------
    gait_walk_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "gait/walk",
        [this](Req, TriggerResponse res) { OnGaitWalk(res); });
    gait_trot_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "gait/trot",
        [this](Req, TriggerResponse res) { OnGaitTrot(res); });
    gait_standard_flat_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "gait/standard_flat",
        [this](Req, TriggerResponse res) { OnGaitStandardFlat(res); });
    gait_standard_stairs_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "gait/standard_stairs",
        [this](Req, TriggerResponse res) { OnGaitStandardStairs(res); });
    gait_agile_flat_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "gait/agile_flat",
        [this](Req, TriggerResponse res) { OnGaitAgileFlat(res); });
    gait_agile_stairs_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "gait/agile_stairs",
        [this](Req, TriggerResponse res) { OnGaitAgileStairs(res); });

    // ---------- 运动状态切换 ----------
    stand_up_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "stand_up",
        [this](Req, TriggerResponse res) { OnStandUp(res); });
    soft_stop_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "soft_stop",
        [this](Req, TriggerResponse res) { OnSoftStop(res); });
    sit_down_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "sit_down",
        [this](Req, TriggerResponse res) { OnSitDown(res); });
    rl_control_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "rl_control",
        [this](Req, TriggerResponse res) { OnRlControl(res); });

    // ---------- 照明灯 ----------
    lights_on_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "lights/on",
        [this](Req, TriggerResponse res) { OnLightsOn(res); });
    lights_off_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "lights/off",
        [this](Req, TriggerResponse res) { OnLightsOff(res); });

    // ---------- 充电 ----------
    charge_start_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "charge/start",
        [this](Req, TriggerResponse res) { OnChargeStart(res); });
    charge_stop_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "charge/stop",
        [this](Req, TriggerResponse res) { OnChargeStop(res); });

    // ---------- 休眠 ----------
    sleep_enter_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "sleep/enter",
        [this](Req, TriggerResponse res) { OnSleepEnter(res); });
    sleep_exit_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "sleep/exit",
        [this](Req, TriggerResponse res) { OnSleepExit(res); });
    sleep_query_srv_ = create_service<std_srvs::srv::Trigger>(
        p + "sleep/query",
        [this](Req, TriggerResponse res) { OnQuerySleepStatus(res); });

    RCLCPP_INFO(get_logger(),
                "Extensions registered: cmd_vel, mode, gait, motion, lights, "
                "charge, sleep");
}

// ===========================================================================
// 内部工具宏：减少重复
// ===========================================================================

#define REQUIRE_CONNECTED(response)                         \
    do {                                                    \
        std::lock_guard<std::mutex> _lk(node_state_mutex_);\
        if (!connected_) {                                  \
            (response)->success = false;                    \
            (response)->message = "Not connected";          \
            return;                                         \
        }                                                   \
    } while (0)

// ===========================================================================
// 模式切换
// ===========================================================================

void LynxAdapterNode::OnModeRegular(TriggerResponse response) {
    SwitchUsageMode(
        LynxSdkClient::kUsageModeRegular, "regular", std::move(response));
}

void LynxAdapterNode::OnModeNavigation(TriggerResponse response) {
    SwitchUsageMode(
        LynxSdkClient::kUsageModeNavigation, "navigation", std::move(response));
}

void LynxAdapterNode::SwitchUsageMode(
    int mode, const char* mode_name, TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    std::lock_guard<std::mutex> control_lock(control_mutex_);

    const auto status = sdk_.GetLatestStatus();
    const bool already_selected =
        IsBasicStatusFresh(status) && status.control_usage_mode == mode;

    std::string error;
    if (!EnsureUsageModeLocked(mode, mode_name, &error)) {
        response->success = false;
        response->message = std::move(error);
        return;
    }

    response->success = true;
    response->message = already_selected
        ? std::string("Mode already set to ") + mode_name
        : std::string("Mode switched to ") + mode_name +
            "; gait was reset by the robot and must be selected again";
    RCLCPP_INFO(get_logger(), "Mode → %s", mode_name);
}

bool LynxAdapterNode::EnsureUsageModeLocked(
    int mode, const char* mode_name, std::string* error) {
    const auto status = sdk_.GetLatestStatus();
    if (IsBasicStatusFresh(status) && status.control_usage_mode == mode) {
        return true;
    }
    if (!SendZeroVelocityForCurrentMode()) {
        *error = "Cannot switch mode: failed to send zero velocity";
        return false;
    }
    last_cmd_vel_time_ = {};

    const auto old_version = sdk_.GetBasicStatusVersion();
    if (!sdk_.SetMode(mode)) {
        *error = std::string("Failed to send ") + mode_name +
            " mode command";
        return false;
    }
    if (!sdk_.WaitForUsageMode(
            old_version, mode,
            std::chrono::milliseconds(control_transition_timeout_ms_))) {
        *error = std::string("Timed out waiting for ") + mode_name +
            " mode confirmation";
        return false;
    }
    if (!SendZeroVelocityForMode(mode)) {
        *error = std::string("Mode changed to ") + mode_name +
            ", but failed to send the matching zero-velocity command";
        return false;
    }
    return true;
}

// ===========================================================================
// 步态切换
// ===========================================================================

void LynxAdapterNode::OnGaitWalk(TriggerResponse response) {
    SwitchGait(
        LynxSdkClient::kGaitStandardFlat,
        "standard flat (legacy walk alias)",
        std::move(response));
}

void LynxAdapterNode::OnGaitTrot(TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    response->success = false;
    response->message =
        "Legacy trot gait (4098) is not supported by the current Lynx protocol; "
        "use gait/standard_flat, gait/standard_stairs, gait/agile_flat, or "
        "gait/agile_stairs";
}

void LynxAdapterNode::OnGaitStandardFlat(TriggerResponse response) {
    SwitchGait(
        LynxSdkClient::kGaitStandardFlat, "standard flat", std::move(response));
}

void LynxAdapterNode::OnGaitStandardStairs(TriggerResponse response) {
    SwitchGait(
        LynxSdkClient::kGaitStandardStairs, "standard stairs", std::move(response));
}

void LynxAdapterNode::OnGaitAgileFlat(TriggerResponse response) {
    SwitchGait(
        LynxSdkClient::kGaitAgileFlat, "agile flat", std::move(response));
}

void LynxAdapterNode::OnGaitAgileStairs(TriggerResponse response) {
    SwitchGait(
        LynxSdkClient::kGaitAgileStairs, "agile stairs", std::move(response));
}

void LynxAdapterNode::SwitchGait(
    int gait, const char* gait_name, TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    std::lock_guard<std::mutex> control_lock(control_mutex_);

    auto status = sdk_.GetLatestStatus();
    if (!IsBasicStatusFresh(status)) {
        response->success = false;
        response->message = "Cannot switch gait: Lynx status is unavailable or stale";
        return;
    }

    const bool is_standard_gait = gait == LynxSdkClient::kGaitStandardFlat ||
        gait == LynxSdkClient::kGaitStandardStairs;
    const bool is_agile_gait = gait == LynxSdkClient::kGaitAgileFlat ||
        gait == LynxSdkClient::kGaitAgileStairs;
    if (!is_standard_gait && !is_agile_gait) {
        response->success = false;
        response->message = "Cannot switch gait: unsupported gait value";
        return;
    }
    if (status.control_usage_mode != LynxSdkClient::kUsageModeRegular &&
        status.control_usage_mode != LynxSdkClient::kUsageModeNavigation &&
        status.control_usage_mode != LynxSdkClient::kUsageModeAssisted) {
        response->success = false;
        response->message = "Cannot switch gait: unknown Lynx usage mode";
        return;
    }

    // 面向用户的敏捷步态是一个高层串行操作：当前为常规模式时，
    // 在内部先切到导航模式。辅助模式本身支持敏捷步态，不强制改变。
    std::string preparation_error;
    if (is_agile_gait &&
        status.control_usage_mode == LynxSdkClient::kUsageModeRegular &&
        !EnsureUsageModeLocked(
            LynxSdkClient::kUsageModeNavigation,
            "navigation",
            &preparation_error)) {
        response->success = false;
        response->message = "Cannot prepare agile gait: " + preparation_error;
        return;
    }

    // RL 控制是本体的步态切换前置状态，由 adapter 内部准备，
    // 不再要求业务用户额外调用 rl_control。
    if (!EnsureMotionStateLocked(
            LynxSdkClient::kMotionStateRlControl,
            "rl_control",
            &preparation_error)) {
        response->success = false;
        response->message = "Cannot prepare gait: " + preparation_error;
        return;
    }

    status = sdk_.GetLatestStatus();
    if (!IsBasicStatusFresh(status)) {
        response->success = false;
        response->message =
            "Cannot switch gait: status became stale after preparing control state";
        return;
    }
    if (status.gait == gait) {
        response->success = true;
        response->message = std::string("Gait already set to ") + gait_name +
            "; required control state is ready";
        return;
    }

    if (!SendZeroVelocityForCurrentMode()) {
        response->success = false;
        response->message = "Cannot switch gait: failed to send zero velocity";
        return;
    }
    last_cmd_vel_time_ = {};

    const auto old_version = sdk_.GetBasicStatusVersion();
    if (!sdk_.SetGait(gait)) {
        response->success = false;
        response->message = std::string("Failed to send ") + gait_name + " gait command";
        return;
    }
    if (!sdk_.WaitForGait(
            old_version, gait,
            std::chrono::milliseconds(control_transition_timeout_ms_))) {
        response->success = false;
        response->message = std::string("Timed out waiting for ") + gait_name +
            " gait confirmation";
        return;
    }

    response->success = true;
    response->message = std::string("Gait switched to ") + gait_name + " (" +
        std::to_string(gait) + "); required control state is ready";
}

// ===========================================================================
// 运动状态切换
// ===========================================================================

void LynxAdapterNode::OnStandUp(TriggerResponse response) {
    SwitchMotionState(
        LynxSdkClient::kMotionStateStanding, "stand_up", std::move(response));
}

void LynxAdapterNode::OnSoftStop(TriggerResponse response) {
    SwitchMotionState(
        LynxSdkClient::kMotionStateSoftStop, "soft_stop", std::move(response));
}

void LynxAdapterNode::OnSitDown(TriggerResponse response) {
    SwitchMotionState(
        LynxSdkClient::kMotionStateProne, "sit_down", std::move(response));
}

void LynxAdapterNode::OnRlControl(TriggerResponse response) {
    SwitchMotionState(
        LynxSdkClient::kMotionStateRlControl, "rl_control", std::move(response));
}

void LynxAdapterNode::SwitchMotionState(
    int motion_state,
    const char* motion_state_name,
    TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    std::lock_guard<std::mutex> control_lock(control_mutex_);

    const auto status = sdk_.GetLatestStatus();
    const bool already_selected =
        IsBasicStatusFresh(status) && status.motion_state == motion_state;

    std::string error;
    if (!EnsureMotionStateLocked(motion_state, motion_state_name, &error)) {
        response->success = false;
        response->message = std::move(error);
        return;
    }

    response->success = true;
    response->message = already_selected
        ? std::string("Motion state already set to ") + motion_state_name
        : std::string("Motion state switched to ") + motion_state_name + " (" +
            std::to_string(motion_state) + ")";
    RCLCPP_INFO(get_logger(), "Motion state → %s", motion_state_name);
}

bool LynxAdapterNode::EnsureMotionStateLocked(
    int motion_state,
    const char* motion_state_name,
    std::string* error) {
    const auto status = sdk_.GetLatestStatus();
    if (IsBasicStatusFresh(status) && status.motion_state == motion_state) {
        return true;
    }
    if (!SendZeroVelocityForCurrentMode()) {
        *error = "Cannot switch motion state: failed to send zero velocity";
        return false;
    }
    last_cmd_vel_time_ = {};

    const auto old_version = sdk_.GetBasicStatusVersion();
    if (!sdk_.SetMotionState(motion_state)) {
        *error = std::string("Failed to send ") + motion_state_name +
            " motion-state command";
        return false;
    }
    if (!sdk_.WaitForMotionState(
            old_version, motion_state,
            std::chrono::milliseconds(control_transition_timeout_ms_))) {
        *error = std::string("Timed out waiting for ") +
            motion_state_name + " motion-state confirmation";
        return false;
    }
    return true;
}

// ===========================================================================
// 照明灯
// ===========================================================================

void LynxAdapterNode::OnLightsOn(TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    sdk_.SetLights(1, 1);
    response->success = true;
    response->message = "Lights on (front=1, back=1)";
}

void LynxAdapterNode::OnLightsOff(TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    sdk_.SetLights(0, 0);
    response->success = true;
    response->message = "Lights off (front=0, back=0)";
}

// ===========================================================================
// 充电
// ===========================================================================

void LynxAdapterNode::OnChargeStart(TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    sdk_.SetAutoCharge(1);
    response->success = true;
    response->message = "Auto charge started";
}

void LynxAdapterNode::OnChargeStop(TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    sdk_.SetAutoCharge(0);
    response->success = true;
    response->message = "Auto charge stopped";
}

// ===========================================================================
// 休眠
// ===========================================================================

void LynxAdapterNode::OnSleepEnter(TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    sdk_.SetSleepMode(true, false, 0);
    response->success = true;
    response->message = "Sleep mode entered";
}

void LynxAdapterNode::OnSleepExit(TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    sdk_.SetSleepMode(false, false, 0);
    response->success = true;
    response->message = "Sleep mode exited";
}

void LynxAdapterNode::OnQuerySleepStatus(TriggerResponse response) {
    REQUIRE_CONNECTED(response);
    const auto old_version = sdk_.GetSleepStatusVersion();
    if (!sdk_.QuerySleepStatus()) {
        response->success = false;
        response->message = "Failed to send sleep status query";
        return;
    }
    if (!sdk_.WaitForSleepStatusUpdate(
            old_version, std::chrono::milliseconds(query_timeout_ms_))) {
        response->success = false;
        response->message = "Sleep status query timed out";
        return;
    }
    auto s = sdk_.GetLatestStatus();
    nlohmann::json j;
    j["sleep_auto"]          = s.sleep_auto;
    j["sleep_auto_time_min"] = s.sleep_auto_time_min;
    response->success = true;
    response->message = j.dump();
}

bool LynxAdapterNode::IsBasicStatusFresh(const LynxStatus& status) const {
    if (!status.valid ||
        status.basic_status_last_update.time_since_epoch().count() == 0) {
        return false;
    }
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - status.basic_status_last_update);
    return age.count() <= status_stale_timeout_ms_;
}

bool LynxAdapterNode::SendZeroVelocityForMode(int mode) {
    if (mode == LynxSdkClient::kUsageModeRegular) {
        return sdk_.SendZeroAxisVelocity();
    }
    if (mode == LynxSdkClient::kUsageModeNavigation) {
        return sdk_.SendZeroAbsoluteVelocity();
    }

    // 辅助模式不接受本节两种速度控制。模式未知或为辅助模式时，
    // 同时尝试清空两条速度通道，避免上一模式的命令残留。
    const bool axis_sent = sdk_.SendZeroAxisVelocity();
    const bool absolute_sent = sdk_.SendZeroAbsoluteVelocity();
    return axis_sent && absolute_sent;
}

bool LynxAdapterNode::SendZeroVelocityForCurrentMode() {
    const auto status = sdk_.GetLatestStatus();
    const int mode = IsBasicStatusFresh(status)
        ? status.control_usage_mode
        : -1;
    return SendZeroVelocityForMode(mode);
}

// ===========================================================================
// cmd_vel Topic 回调：入口始终是 SI 单位，根据当前使用模式选择底层指令
//   常规模式(0) -> 限速 + 归一化 -> Command=21
//   导航模式(1) -> 仅限速             -> Command=25
// ===========================================================================

void LynxAdapterNode::OnCmdVel(const geometry_msgs::msg::Twist::SharedPtr msg) {
    {
        std::lock_guard<std::mutex> lock(node_state_mutex_);
        if (!connected_) return;
    }
    std::lock_guard<std::mutex> control_lock(control_mutex_);

    double vx = msg->linear.x;
    double vy = msg->linear.y;
    double wz = msg->angular.z;

    constexpr double kDeadZone = 1e-4;
    if (std::abs(vx) < kDeadZone) vx = 0.0;
    if (std::abs(vy) < kDeadZone) vy = 0.0;
    if (std::abs(wz) < kDeadZone) wz = 0.0;

    const auto status = sdk_.GetLatestStatus();
    if (!IsBasicStatusFresh(status)) {
        RCLCPP_WARN_THROTTLE(
            get_logger(), *get_clock(), 2000,
            "Rejected cmd_vel: Lynx basic status is unavailable or stale");
        last_cmd_vel_time_ = {};
        return;
    }
    if (status.motion_state != LynxSdkClient::kMotionStateRlControl) {
        RCLCPP_WARN_THROTTLE(
            get_logger(), *get_clock(), 2000,
            "Rejected cmd_vel: RL control state (17) is required; current=%d",
            status.motion_state);
        last_cmd_vel_time_ = {};
        return;
    }

    bool sent = false;
    if (status.control_usage_mode == LynxSdkClient::kUsageModeRegular) {
        const auto command = velocity_converter_.Convert(vx, vy, wz);
        if (command.has_value()) {
            sent = sdk_.SendMotionCmd(
                command->x_ratio, command->y_ratio, command->yaw_ratio);
        }
    } else if (
        status.control_usage_mode == LynxSdkClient::kUsageModeNavigation) {
        const auto command = velocity_converter_.Limit(vx, vy, wz);
        if (command.has_value()) {
            sent = sdk_.SendVelocityCmd(
                command->linear_x_mps,
                command->linear_y_mps,
                command->angular_z_radps);
        }
    } else {
        RCLCPP_WARN_THROTTLE(
            get_logger(), *get_clock(), 2000,
            "Rejected cmd_vel: usage mode %d does not support velocity commands",
            status.control_usage_mode);
        SendZeroVelocityForMode(status.control_usage_mode);
        last_cmd_vel_time_ = {};
        return;
    }

    if (!sent) {
        RCLCPP_ERROR(
            get_logger(),
            "Rejected or failed to send cmd_vel in usage mode %d; sending zero velocity",
            status.control_usage_mode);
        SendZeroVelocityForMode(status.control_usage_mode);
        last_cmd_vel_time_ = {};
        return;
    }
    last_cmd_vel_time_ = std::chrono::steady_clock::now();
}

// ===========================================================================
// 看门狗：cmd_vel 超时则自动发零速
// ===========================================================================

void LynxAdapterNode::OnWatchdogTick() {
    {
        std::lock_guard<std::mutex> lock(node_state_mutex_);
        if (!connected_) return;
    }
    std::lock_guard<std::mutex> control_lock(control_mutex_);

    if (last_cmd_vel_time_.time_since_epoch().count() == 0) return;

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_cmd_vel_time_).count();

    if (elapsed_ms > cmd_vel_timeout_ms_) {
        const bool sent = SendZeroVelocityForCurrentMode();
        last_cmd_vel_time_ = {};
        if (sent) {
            RCLCPP_WARN(get_logger(),
                        "cmd_vel timeout (%lld ms), sent zero velocity",
                        static_cast<long long>(elapsed_ms));
        } else {
            RCLCPP_ERROR(get_logger(),
                         "cmd_vel timeout (%lld ms), failed to send zero velocity",
                         static_cast<long long>(elapsed_ms));
        }
    }
}

}  // namespace adapter_lynx
