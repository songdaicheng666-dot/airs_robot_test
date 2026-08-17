#include "adapter_scout/scout_adapter_node.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>
#include <robot_adapter_interfaces/system_info.hpp>

namespace adapter_scout {

namespace {

constexpr double kUnsupportedAxisWarningThreshold = 1e-6;

bool HasUnsupportedVelocity(const geometry_msgs::msg::Twist& message) {
    return std::abs(message.linear.y) > kUnsupportedAxisWarningThreshold ||
        std::abs(message.linear.z) > kUnsupportedAxisWarningThreshold ||
        std::abs(message.angular.x) > kUnsupportedAxisWarningThreshold ||
        std::abs(message.angular.y) > kUnsupportedAxisWarningThreshold;
}

}  // namespace

ScoutAdapterNode::ScoutAdapterNode()
    : AdapterNodeBase("scout") {
    can_interface_ =
        GetParamOrDefault<std::string>("can_interface", "can0");
    const double max_linear_x_mps =
        GetParamOrDefault<double>("max_linear_x_mps", 1.5);
    const double max_angular_z_radps =
        GetParamOrDefault<double>("max_angular_z_radps", 1.3075);
    codec_ = ScoutCommandCodec(max_linear_x_mps, max_angular_z_radps);
    cmd_vel_timeout_ms_ =
        GetParamOrDefault<int>("cmd_vel_timeout_ms", 500);
    watchdog_check_interval_ms_ =
        GetParamOrDefault<int>("watchdog_check_interval_ms", 100);

    if (can_interface_.empty()) {
        throw std::invalid_argument("can_interface must not be empty");
    }
    if (cmd_vel_timeout_ms_ <= 0) {
        throw std::invalid_argument("cmd_vel_timeout_ms must be positive");
    }
    if (watchdog_check_interval_ms_ <= 0) {
        throw std::invalid_argument(
            "watchdog_check_interval_ms must be positive");
    }
    if (watchdog_check_interval_ms_ > cmd_vel_timeout_ms_) {
        throw std::invalid_argument(
            "watchdog_check_interval_ms must not exceed cmd_vel_timeout_ms");
    }

    RCLCPP_INFO(
        get_logger(),
        "adapter_scout ready: SocketCAN=%s, limits x=%.3f m/s yaw=%.4f rad/s",
        can_interface_.c_str(), max_linear_x_mps, max_angular_z_radps);
}

ScoutAdapterNode::~ScoutAdapterNode() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (connected_) {
        std::string ignored_error;
        (void)can_client_.Send(
            codec_.BuildZeroVelocityFrame(), &ignored_error);
    }
    connected_ = false;
    ResetWatchdogLocked();
    can_client_.Disconnect();
}

void ScoutAdapterNode::OnConnect(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(control_mutex_);

    if (connected_) {
        response->success = true;
        response->message = "Already connected";
        return;
    }

    ResetWatchdogLocked();
    std::string error;
    if (!can_client_.Connect(can_interface_, &error)) {
        transport_fault_ = true;
        last_error_ = "SocketCAN connect failed: " + error;
        response->success = false;
        response->message = last_error_;
        RCLCPP_ERROR_THROTTLE(
            get_logger(), *get_clock(), 2000, "%s", last_error_.c_str());
        return;
    }

    if (!SendFrameLocked(
            ScoutCommandCodec::BuildCanModeFrame(),
            "failed to enable Scout CAN mode",
            &error) ||
        !SendFrameLocked(
            codec_.BuildZeroVelocityFrame(),
            "failed to send initial zero velocity",
            &error)) {
        can_client_.Disconnect();
        response->success = false;
        response->message = error;
        RCLCPP_ERROR_THROTTLE(
            get_logger(), *get_clock(), 2000,
            "Connect failed: %s", error.c_str());
        return;
    }

    connected_ = true;
    transport_fault_ = false;
    last_error_.clear();
    response->success = true;
    response->message = "Connected to Scout through SocketCAN " + can_interface_;
    RCLCPP_INFO(
        get_logger(), "Connected to Scout through %s", can_interface_.c_str());
}

void ScoutAdapterNode::OnDisconnect(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(control_mutex_);

    if (!connected_) {
        can_client_.Disconnect();
        ResetWatchdogLocked();
        response->success = true;
        response->message = "Already disconnected";
        return;
    }

    connected_ = false;
    ResetWatchdogLocked();

    std::string error;
    const bool zero_sent = SendFrameLocked(
        codec_.BuildZeroVelocityFrame(),
        "disconnect failed to send zero velocity",
        &error);
    can_client_.Disconnect();

    response->success = zero_sent;
    response->message = zero_sent ? "Disconnected" : error;
    if (zero_sent) {
        RCLCPP_INFO(get_logger(), "Disconnected from Scout");
    } else {
        RCLCPP_ERROR(get_logger(), "%s; SocketCAN was closed", error.c_str());
    }
}

void ScoutAdapterNode::OnSafeStop(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(control_mutex_);

    ResetWatchdogLocked();
    if (!connected_) {
        response->success = true;
        response->message = "Not connected, nothing to stop";
        return;
    }

    std::string error;
    const bool sent = SendFrameLocked(
        codec_.BuildZeroVelocityFrame(),
        "safe_stop failed to send zero velocity",
        &error);
    response->success = sent;
    response->message = sent ? "safe_stop: zero velocity sent" : error;
    if (!sent) {
        RCLCPP_ERROR(get_logger(), "%s", error.c_str());
    }
}

void ScoutAdapterNode::OnHealth(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(control_mutex_);

    std::string interface_error;
    const bool interface_up = ScoutCanClient::CheckInterfaceUp(
        can_interface_, &interface_error);
    const bool socket_open = can_client_.IsConnected();
    const bool transport_ready =
        connected_ && socket_open && interface_up && !transport_fault_;

    nlohmann::json health = {
        {"connected", connected_},
        {"transport_ready", transport_ready},
        {"transport", "socketcan"},
        {"can_interface", can_interface_},
        {"interface_up", interface_up},
        {"socket_open", socket_open},
        {"last_error", last_error_},
    };
    if (!interface_error.empty()) {
        health["interface_error"] = interface_error;
    }

    response->success = transport_ready;
    response->message = health.dump();
}

void ScoutAdapterNode::OnSystemInfo(TriggerResponse response) {
    std::lock_guard<std::mutex> lock(control_mutex_);

    std::string interface_error;
    const bool interface_up = ScoutCanClient::CheckInterfaceUp(
        can_interface_, &interface_error);
    const bool socket_open = can_client_.IsConnected();
    const bool transport_ready =
        connected_ && socket_open && interface_up && !transport_fault_;

    nlohmann::json details = {
        {"adapter", "scout"},
        {"transport", "socketcan"},
        {"can_interface", can_interface_},
        {"connected", connected_},
        {"transport_ready", transport_ready},
        {"interface_up", interface_up},
        {"socket_open", socket_open},
        {"last_error", last_error_},
    };
    if (!interface_error.empty()) {
        details["interface_error"] = interface_error;
    }

    robot_adapter_interfaces::SystemInfoBuilder system_info;
    system_info.SetDetailsJson(details.dump());
    response->success = transport_ready;
    response->message = system_info.Build();
}

void ScoutAdapterNode::RegisterExtensions() {
    cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
        GetCmdVelTopic(), 10,
        [this](const geometry_msgs::msg::Twist::SharedPtr message) {
            OnCmdVel(message);
        });
    watchdog_timer_ = create_wall_timer(
        std::chrono::milliseconds(watchdog_check_interval_ms_),
        [this]() { OnWatchdogTick(); });

    RCLCPP_INFO(
        get_logger(),
        "Scout extensions registered: cmd_vel and %d ms watchdog",
        cmd_vel_timeout_ms_);
}

void ScoutAdapterNode::OnCmdVel(
    const geometry_msgs::msg::Twist::SharedPtr msg) {
    std::lock_guard<std::mutex> lock(control_mutex_);

    if (!connected_) {
        RCLCPP_WARN_THROTTLE(
            get_logger(), *get_clock(), 2000,
            "Ignoring cmd_vel while Scout is disconnected");
        return;
    }
    if (HasUnsupportedVelocity(*msg)) {
        RCLCPP_WARN_THROTTLE(
            get_logger(), *get_clock(), 2000,
            "Scout ignores linear.y, linear.z, angular.x and angular.y");
    }

    std::string error;
    const auto command = codec_.EncodeVelocity(
        msg->linear.x, msg->angular.z, &error);
    if (!command.has_value()) {
        RCLCPP_WARN_THROTTLE(
            get_logger(), *get_clock(), 2000,
            "Rejected invalid Scout cmd_vel: %s", error.c_str());
        return;
    }
    if (command->limited) {
        RCLCPP_WARN_THROTTLE(
            get_logger(), *get_clock(), 2000,
            "Scout cmd_vel was limited to protocol safety bounds");
    }

    if (!SendFrameLocked(command->frame, "failed to send cmd_vel", &error)) {
        RCLCPP_ERROR_THROTTLE(
            get_logger(), *get_clock(), 2000, "%s", error.c_str());
        return;
    }

    if (command->IsZero()) {
        ResetWatchdogLocked();
    } else {
        watchdog_armed_ = true;
        last_cmd_vel_time_ = std::chrono::steady_clock::now();
    }
}

void ScoutAdapterNode::OnWatchdogTick() {
    std::lock_guard<std::mutex> lock(control_mutex_);

    if (!connected_ || !watchdog_armed_) {
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - last_cmd_vel_time_);
    if (elapsed.count() < cmd_vel_timeout_ms_) {
        return;
    }

    ResetWatchdogLocked();
    std::string error;
    if (SendFrameLocked(
            codec_.BuildZeroVelocityFrame(),
            "cmd_vel watchdog failed to send zero velocity",
            &error)) {
        RCLCPP_WARN(
            get_logger(),
            "cmd_vel timeout (%lld ms), sent zero velocity",
            static_cast<long long>(elapsed.count()));
    } else {
        RCLCPP_ERROR(get_logger(), "%s", error.c_str());
    }
}

bool ScoutAdapterNode::SendFrameLocked(
    const ScoutCanFrame& frame,
    const std::string& context,
    std::string* error) {
    std::string transport_error;
    if (!can_client_.Send(frame, &transport_error)) {
        transport_fault_ = true;
        last_error_ = context + ": " + transport_error;
        if (error != nullptr) {
            *error = last_error_;
        }
        return false;
    }

    transport_fault_ = false;
    last_error_.clear();
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void ScoutAdapterNode::ResetWatchdogLocked() {
    watchdog_armed_ = false;
    last_cmd_vel_time_ = {};
}

}  // namespace adapter_scout
