#include "adapter_go2/go2_adapter_node.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <nlohmann/json.hpp>
#include <robot_adapter_interfaces/system_info.hpp>

namespace adapter_go2 {

namespace {

constexpr float kVelocityDeadband = 0.005f;

}  // namespace

Go2AdapterNode::Go2AdapterNode()
    : AdapterNodeBase("go2") {
    network_interface_ = GetRequiredParam<std::string>("network_interface");
    sdk_timeout_sec_ = GetParamOrDefault<double>("sdk_timeout_sec", 10.0);
    auto_stand_on_connect_ = GetParamOrDefault<bool>("auto_stand_on_connect", true);
    stand_down_on_disconnect_ =
        GetParamOrDefault<bool>("stand_down_on_disconnect", false);
    max_linear_x_ = GetParamOrDefault<double>("max_linear_x", 1.5);
    max_linear_y_ = GetParamOrDefault<double>("max_linear_y", 1.0);
    max_angular_z_ = GetParamOrDefault<double>("max_angular_z", 2.0);
    cmd_vel_timeout_ms_ = GetParamOrDefault<int>("cmd_vel_timeout_ms", 500);
    watchdog_check_interval_ms_ =
        GetParamOrDefault<int>("watchdog_check_interval_ms", 100);
    safe_stop_action_ = GetParamOrDefault<std::string>("safe_stop_action", "stop_move");
    connect_state_timeout_ms_ =
        GetParamOrDefault<int>("connect_state_timeout_ms", 1000);
    state_stale_timeout_ms_ =
        GetParamOrDefault<int>("state_stale_timeout_ms", 2000);

    sdk_.SetSportStateCallback([this](const auto& state) { OnSportState(state); });
    sdk_.SetLowStateCallback([this](const auto& state) { OnLowState(state); });

    RCLCPP_INFO(
        get_logger(),
        "adapter_go2 started. iface=%s auto_stand=%s cmd_vel_timeout=%dms safe_stop=%s",
        network_interface_.c_str(),
        auto_stand_on_connect_ ? "true" : "false",
        cmd_vel_timeout_ms_,
        safe_stop_action_.c_str());
}

void Go2AdapterNode::RegisterExtensions() {
    const std::string prefix = "/" + std::string(get_name()) + "/";

    stand_up_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "stand_up",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnStandUp(response); });

    stop_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "stop",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnStop(response); });

    emergency_stop_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "emergency_stop",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnEmergencyStop(response); });

    sit_down_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "sit_down",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnSitDown(response); });

    cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
        GetCmdVelTopic(),
        10,
        [this](const geometry_msgs::msg::Twist::SharedPtr message) { OnCmdVel(message); });

    watchdog_timer_ = create_wall_timer(
        std::chrono::milliseconds(watchdog_check_interval_ms_),
        [this]() { OnWatchdogTick(); });
}

bool Go2AdapterNode::EnsureSdkInitialized(TriggerResponse response) {
    Go2SdkConfig config{network_interface_, sdk_timeout_sec_};
    std::string init_error;
    if (!sdk_.Initialize(config, &init_error)) {
        response->success = false;
        response->message = init_error;
        return false;
    }
    return true;
}

bool Go2AdapterNode::IsConnectedSnapshot() const {
    std::lock_guard<std::mutex> lock(node_state_mutex_);
    return connected_;
}

Go2AdapterNode::ControlState Go2AdapterNode::GetControlStateSnapshot() const {
    std::lock_guard<std::mutex> lock(node_state_mutex_);
    return control_state_;
}

void Go2AdapterNode::OnConnect(TriggerResponse response) {
    if (!EnsureSdkInitialized(response)) return;

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);

    {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        if (connected_) {
            response->success = true;
            response->message = "GO2 already connected";
            return;
        }
    }

    // Reset state flags before connect — prevents stale data from prior session
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        has_sport_state_ = false;
        has_low_state_ = false;
        last_sport_state_time_ = {};
        last_low_state_time_ = {};
    }

    try {
        if (auto_stand_on_connect_) {
            const int32_t stand_ret = sdk_.RecoveryStand();
            if (stand_ret != 0) {
                std::lock_guard<std::mutex> state_lock(node_state_mutex_);
                last_error_ = "stand command failed, ret=" + std::to_string(stand_ret);
                response->success = false;
                response->message = "GO2 connect failed: " + last_error_;
                return;
            }
        }

        const int32_t stop_ret = sdk_.StopMove();
        if (stop_ret != 0) {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            last_error_ = "StopMove during connect failed, ret=" + std::to_string(stop_ret);
            response->success = false;
            response->message = "GO2 connect failed: " + last_error_;
            return;
        }

        // Wait for at least one fresh sport_state frame
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(connect_state_timeout_ms_);
        bool received_sport = false;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> state_lock(state_mutex_);
                if (has_sport_state_) {
                    received_sport = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        if (!received_sport) {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            last_error_ = "no sport_state received within " +
                std::to_string(connect_state_timeout_ms_) + "ms";
            response->success = false;
            response->message = "GO2 connect failed: " + last_error_;
            return;
        }

        {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            connected_ = true;
            control_state_ = ControlState::kConnectedIdle;
            last_error_.clear();
        }
        response->success = true;
        response->message = "GO2 connected, iface=" + network_interface_;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        last_error_ = std::string("GO2 connect exception: ") + e.what();
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnDisconnect(TriggerResponse response) {
    if (!EnsureSdkInitialized(response)) return;

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);

    {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        if (!connected_) {
            response->success = true;
            response->message = "GO2 already disconnected";
            return;
        }
    }

    try {
        const int32_t stop_ret = sdk_.StopMove();
        if (stop_ret != 0) {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            last_error_ = "disconnect: StopMove failed, ret=" + std::to_string(stop_ret);
            response->success = false;
            response->message = last_error_;
            return;
        }

        if (stand_down_on_disconnect_) {
            const int32_t stand_down_ret = sdk_.StandDown();
            if (stand_down_ret != 0) {
                std::lock_guard<std::mutex> state_lock(node_state_mutex_);
                last_error_ =
                    "disconnect: StandDown failed, ret=" + std::to_string(stand_down_ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
        }

        {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            connected_ = false;
            control_state_ = ControlState::kDisconnected;
        }

        response->success = true;
        response->message = "GO2 disconnected";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        connected_ = false;
        control_state_ = ControlState::kFault;
        last_error_ = std::string("GO2 disconnect exception: ") + e.what();
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnSafeStop(TriggerResponse response) {
    if (!IsConnectedSnapshot()) {
        response->success = true;
        response->message = "not connected; safe_stop is a no-op";
        return;
    }

    if (!EnsureSdkInitialized(response)) return;

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);
    try {
        if (safe_stop_action_ == "damp") {
            const int32_t ret = sdk_.Damp();
            if (ret != 0) {
                std::lock_guard<std::mutex> state_lock(node_state_mutex_);
                last_error_ = "safe_stop damp failed, ret=" + std::to_string(ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
        } else if (safe_stop_action_ == "stop_and_sit") {
            const int32_t stop_ret = sdk_.StopMove();
            if (stop_ret != 0) {
                std::lock_guard<std::mutex> state_lock(node_state_mutex_);
                last_error_ = "safe_stop StopMove failed, ret=" + std::to_string(stop_ret);
                response->success = false;
                response->message = last_error_;
                return;
            }

            const int32_t sit_ret = sdk_.StandDown();
            if (sit_ret != 0) {
                std::lock_guard<std::mutex> state_lock(node_state_mutex_);
                last_error_ = "safe_stop StandDown failed, ret=" + std::to_string(sit_ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
        } else {
            const int32_t ret = sdk_.StopMove();
            if (ret != 0) {
                std::lock_guard<std::mutex> state_lock(node_state_mutex_);
                last_error_ = "safe_stop StopMove failed, ret=" + std::to_string(ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
        }

        {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            control_state_ = ControlState::kConnectedIdle;
        }
        response->success = true;
        response->message = "safe_stop success (action=" + safe_stop_action_ + ")";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        last_error_ = std::string("safe_stop exception: ") + e.what();
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnHealth(TriggerResponse response) {
    if (!EnsureSdkInitialized(response)) return;

    std::vector<unitree::robot::go2::ServiceState> service_list;
    int32_t service_ret = -1;
    try {
        service_ret = sdk_.GetServiceList(service_list);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        last_error_ = std::string("ServiceList exception: ") + e.what();
    }

    bool has_sport = false;
    bool has_low = false;
    int64_t sport_stale_ms = -1;
    int64_t low_stale_ms = -1;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        has_sport = has_sport_state_;
        has_low = has_low_state_;
        const auto now = std::chrono::steady_clock::now();
        if (has_sport) {
            sport_stale_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_sport_state_time_).count();
        }
        if (has_low) {
            low_stale_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_low_state_time_).count();
        }
    }

    bool snapshot_connected = false;
    ControlState snapshot_control_state = ControlState::kDisconnected;
    std::string snapshot_last_error;
    std::chrono::steady_clock::time_point snapshot_last_cmd_vel;
    {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        snapshot_connected = connected_;
        snapshot_control_state = control_state_;
        snapshot_last_error = last_error_;
        snapshot_last_cmd_vel = last_cmd_vel_time_;
    }

    const bool sport_fresh = has_sport && sport_stale_ms <= state_stale_timeout_ms_;
    const bool service_ok = service_ret == 0;

    nlohmann::json data;
    data["connected"] = snapshot_connected;
    data["iface"] = network_interface_;
    data["service_list_ret"] = service_ret;
    data["service_count"] = service_list.size();
    data["has_sport_state"] = has_sport;
    data["has_low_state"] = has_low;
    if (has_sport) data["sport_stale_ms"] = sport_stale_ms;
    if (has_low) data["low_stale_ms"] = low_stale_ms;
    data["sport_fresh"] = sport_fresh;
    data["cmd_vel_timeout_ms"] = cmd_vel_timeout_ms_;

    const char* control_state_string = "unknown";
    switch (snapshot_control_state) {
    case ControlState::kDisconnected:
        control_state_string = "disconnected";
        break;
    case ControlState::kConnectedIdle:
        control_state_string = "connected_idle";
        break;
    case ControlState::kConnectedCommanding:
        control_state_string = "connected_commanding";
        break;
    case ControlState::kFault:
        control_state_string = "fault";
        break;
    }
    data["control_state"] = control_state_string;

    if (snapshot_connected && snapshot_last_cmd_vel.time_since_epoch().count() > 0) {
        const auto ms_since_last_cmd_vel = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - snapshot_last_cmd_vel);
        data["ms_since_last_cmd_vel"] = ms_since_last_cmd_vel.count();
    }

    if (!snapshot_last_error.empty()) {
        data["last_error"] = snapshot_last_error;
    }

    // Health is ok only when: connected, service reachable, sport_state fresh
    response->success = snapshot_connected && service_ok && sport_fresh;
    response->message = data.dump();
}

void Go2AdapterNode::OnSystemInfo(TriggerResponse response) {
    if (!EnsureSdkInitialized(response)) return;

    robot_adapter_interfaces::SystemInfoBuilder system_info;
    bool has_sport = false;
    bool has_low = false;
    int64_t sport_stale_ms = -1;
    SportStateSnapshot sport_snapshot;
    LowStateSnapshot low_snapshot;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        has_sport = has_sport_state_;
        has_low = has_low_state_;
        sport_snapshot = latest_sport_state_;
        low_snapshot = latest_low_state_;
        if (has_sport) {
            sport_stale_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - last_sport_state_time_).count();
        }
    }

    const bool snapshot_connected = IsConnectedSnapshot();
    const bool sport_fresh = has_sport && sport_stale_ms <= state_stale_timeout_ms_;

    std::vector<unitree::robot::go2::ServiceState> services;
    int32_t service_ret = -1;
    try {
        service_ret = sdk_.GetServiceList(services);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        last_error_ = std::string("ServiceList exception: ") + e.what();
    }

    nlohmann::json data;
    data["connected"] = snapshot_connected;
    data["network_interface"] = network_interface_;
    data["has_sport_state"] = has_sport;
    data["has_low_state"] = has_low;
    if (has_sport) data["sport_stale_ms"] = sport_stale_ms;
    data["sport_fresh"] = sport_fresh;
    data["service_list_ret"] = service_ret;
    data["service_count"] = services.size();

    if (has_sport && sport_fresh) {
        const auto& velocity = sport_snapshot.velocity;
        system_info.SetMotion(velocity[0], velocity[1], velocity[2]);
        data["sport"] = {
            {"error_code", sport_snapshot.error_code},
            {"mode", sport_snapshot.mode},
            {"gait_type", sport_snapshot.gait_type},
            {"velocity",
             {velocity[0], velocity[1], velocity[2]}},
        };
    } else {
        data["sport"]["error"] = has_sport ? "sport state stale" : "no sport state yet";
    }

    if (has_low) {
        system_info.SetBattery(low_snapshot.battery_soc);
        data["low"] = {
            {"battery_soc", low_snapshot.battery_soc},
            {"battery_current", low_snapshot.battery_current},
            {"battery_cycle", low_snapshot.battery_cycle},
            {"battery_status", low_snapshot.battery_status},
            {"battery_version",
             std::to_string(low_snapshot.battery_version_high) + "." +
                 std::to_string(low_snapshot.battery_version_low)},
            {"battery_bq_ntc", low_snapshot.battery_bq_ntc},
            {"battery_mcu_ntc", low_snapshot.battery_mcu_ntc},
            {"power_v", low_snapshot.power_v},
            {"power_a", low_snapshot.power_a},
        };
    } else {
        data["low"]["error"] = "no low state yet";
    }

    nlohmann::json services_json = nlohmann::json::array();
    services_json.get_ptr<nlohmann::json::array_t*>()->reserve(services.size());
    for (const auto& service : services) {
        services_json.push_back(
            {{"name", service.name}, {"status", service.status}, {"protect", service.protect}});
    }
    data["services"] = services_json;

    system_info.SetMotions({
        {"stand_up", "stand_up", "Recover to standing posture", "站立"},
        {"stop", "stop", "Halt in place", "停止"},
        {"sit_down", "sit_down", "Stop motion then stand down", "趴下"},
        {"emergency_stop", "emergency_stop", "Damp all joints", "急停"},
    });
    system_info.SetDetailsJson(data.dump());
    // Fail if disconnected or sport_state is stale
    response->success = snapshot_connected && sport_fresh;
    response->message = system_info.Build();
}

template <typename Func>
void Go2AdapterNode::ExecuteSportCommand(
    const std::string& command_name,
    Func&& sdk_call,
    TriggerResponse response) {
    if (!EnsureSdkInitialized(response)) return;

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);

    {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        if (!connected_) {
            response->success = false;
            response->message = command_name + " rejected: GO2 not connected";
            return;
        }
    }

    try {
        const int32_t ret = sdk_call();
        if (ret != 0) {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            last_error_ = command_name + " failed, ret=" + std::to_string(ret);
            response->success = false;
            response->message = last_error_;
            return;
        }
        response->success = true;
        response->message = command_name + " success";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        last_error_ = command_name + " exception: " + std::string(e.what());
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnStandUp(TriggerResponse response) {
    ExecuteSportCommand(
        "stand_up",
        [this]() { return sdk_.RecoveryStand(); },
        response);
}

void Go2AdapterNode::OnStop(TriggerResponse response) {
    ExecuteSportCommand(
        "stop",
        [this]() { return sdk_.StopMove(); },
        response);
}

void Go2AdapterNode::OnDamp(TriggerResponse response) {
    ExecuteSportCommand(
        "damp",
        [this]() { return sdk_.Damp(); },
        response);
}

void Go2AdapterNode::OnEmergencyStop(TriggerResponse response) {
    OnDamp(response);
}

void Go2AdapterNode::OnSitDown(TriggerResponse response) {
    if (!EnsureSdkInitialized(response)) return;

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);
    if (!IsConnectedSnapshot()) {
        response->success = false;
        response->message = "sit_down rejected: GO2 not connected";
        return;
    }

    try {
        const int32_t stop_ret = sdk_.StopMove();
        if (stop_ret != 0) {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            last_error_ = "sit_down: stop failed, ret=" + std::to_string(stop_ret);
            response->success = false;
            response->message = last_error_;
            return;
        }

        const int32_t sit_ret = sdk_.StandDown();
        if (sit_ret != 0) {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            last_error_ = "sit_down: stand down failed, ret=" + std::to_string(sit_ret);
            response->success = false;
            response->message = last_error_;
            return;
        }

        response->success = true;
        response->message = "sit_down success";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        last_error_ = std::string("sit_down exception: ") + e.what();
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnCmdVel(const geometry_msgs::msg::Twist::SharedPtr msg) {
    if (msg == nullptr) {
        return;
    }

    if (!sdk_.IsInitialized()) {
        return;
    }

    ControlState snapshot_control_state = ControlState::kDisconnected;
    {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        if (!connected_) {
            return;
        }
        snapshot_control_state = control_state_;
        last_cmd_vel_time_ = std::chrono::steady_clock::now();
    }

    try {
        const float vx = std::clamp(
            static_cast<float>(msg->linear.x),
            -static_cast<float>(max_linear_x_),
            static_cast<float>(max_linear_x_));
        const float vy = std::clamp(
            static_cast<float>(msg->linear.y),
            -static_cast<float>(max_linear_y_),
            static_cast<float>(max_linear_y_));
        const float wz = std::clamp(
            static_cast<float>(msg->angular.z),
            -static_cast<float>(max_angular_z_),
            static_cast<float>(max_angular_z_));

        const bool is_zero_velocity =
            std::abs(vx) < kVelocityDeadband &&
            std::abs(vy) < kVelocityDeadband &&
            std::abs(wz) < kVelocityDeadband;

        if (is_zero_velocity && snapshot_control_state == ControlState::kConnectedIdle) {
            return;
        }

        std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);
        const int32_t ret = is_zero_velocity ? sdk_.StopMove() : sdk_.Move(vx, vy, wz);

        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        control_state_ = is_zero_velocity ? ControlState::kConnectedIdle
                                          : ControlState::kConnectedCommanding;

        if (ret != 0) {
            last_error_ = "cmd_vel failed, ret=" + std::to_string(ret);
            RCLCPP_WARN(
                get_logger(),
                "cmd_vel failed ret=%d raw(vx=%.3f,vy=%.3f,wz=%.3f) clamped(vx=%.3f,vy=%.3f,wz=%.3f)",
                ret,
                msg->linear.x,
                msg->linear.y,
                msg->angular.z,
                vx,
                vy,
                wz);
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        last_error_ = std::string("cmd_vel exception: ") + e.what();
        RCLCPP_ERROR(get_logger(), "%s", last_error_.c_str());
    }
}

void Go2AdapterNode::OnWatchdogTick() {
    // Early exit checks without expensive snapshot
    if (!sdk_.IsInitialized()) {
        return;
    }

    const ControlState state = GetControlStateSnapshot();
    if (state != ControlState::kConnectedCommanding) {
        return;
    }

    std::chrono::steady_clock::time_point last_cmd_time;
    {
        std::lock_guard<std::mutex> lock(node_state_mutex_);
        last_cmd_time = last_cmd_vel_time_;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cmd_time);
    if (elapsed.count() < cmd_vel_timeout_ms_) {
        return;
    }

    RCLCPP_WARN(
        get_logger(),
        "cmd_vel watchdog triggered: no cmd_vel for %ldms, calling StopMove",
        static_cast<long>(elapsed.count()));

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);
    try {
        const int32_t ret = sdk_.StopMove();
        if (ret != 0) {
            std::lock_guard<std::mutex> state_lock(node_state_mutex_);
            last_error_ = "watchdog StopMove failed, ret=" + std::to_string(ret);
            RCLCPP_ERROR(get_logger(), "%s", last_error_.c_str());
            return;
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> state_lock(node_state_mutex_);
        last_error_ = std::string("watchdog StopMove exception: ") + e.what();
        RCLCPP_ERROR(get_logger(), "%s", last_error_.c_str());
        return;
    }

    std::lock_guard<std::mutex> state_lock(node_state_mutex_);
    control_state_ = ControlState::kConnectedIdle;
}

void Go2AdapterNode::OnSportState(const unitree_go::msg::dds_::SportModeState_& state) {
    const auto& velocity = state.velocity();
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_sport_state_.velocity = velocity;
    latest_sport_state_.error_code = state.error_code();
    latest_sport_state_.mode = static_cast<int>(state.mode());
    latest_sport_state_.gait_type = static_cast<int>(state.gait_type());
    has_sport_state_ = true;
    last_sport_state_time_ = std::chrono::steady_clock::now();
}

void Go2AdapterNode::OnLowState(const unitree_go::msg::dds_::LowState_& state) {
    const auto& bms = state.bms_state();
    const auto& bq_ntc = bms.bq_ntc();
    const auto& mcu_ntc = bms.mcu_ntc();

    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_low_state_.battery_soc = static_cast<int>(bms.soc());
    latest_low_state_.battery_current = bms.current();
    latest_low_state_.battery_cycle = bms.cycle();
    latest_low_state_.battery_status = static_cast<int>(bms.status());
    latest_low_state_.battery_version_high = static_cast<int>(bms.version_high());
    latest_low_state_.battery_version_low = static_cast<int>(bms.version_low());
    latest_low_state_.battery_bq_ntc = {
        static_cast<int>(bq_ntc[0]),
        static_cast<int>(bq_ntc[1]),
    };
    latest_low_state_.battery_mcu_ntc = {
        static_cast<int>(mcu_ntc[0]),
        static_cast<int>(mcu_ntc[1]),
    };
    latest_low_state_.power_v = state.power_v();
    latest_low_state_.power_a = state.power_a();
    has_low_state_ = true;
    last_low_state_time_ = std::chrono::steady_clock::now();
}

}  // namespace adapter_go2
