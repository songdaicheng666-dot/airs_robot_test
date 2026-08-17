#pragma once

#include <chrono>
#include <mutex>
#include <string>

#include <geometry_msgs/msg/twist.hpp>
#include <robot_adapter_interfaces/adapter_node_base.hpp>

#include "adapter_scout/scout_can_client.hpp"
#include "adapter_scout/scout_command_codec.hpp"

namespace adapter_scout {

class ScoutAdapterNode : public robot_adapter_interfaces::AdapterNodeBase {
public:
    ScoutAdapterNode();
    ~ScoutAdapterNode() override;

protected:
    void OnConnect(TriggerResponse response) override;
    void OnDisconnect(TriggerResponse response) override;
    void OnSafeStop(TriggerResponse response) override;
    void OnHealth(TriggerResponse response) override;
    void OnSystemInfo(TriggerResponse response) override;
    void RegisterExtensions() override;

private:
    void OnCmdVel(const geometry_msgs::msg::Twist::SharedPtr msg);
    void OnWatchdogTick();
    bool SendFrameLocked(
        const ScoutCanFrame& frame,
        const std::string& context,
        std::string* error = nullptr);
    void ResetWatchdogLocked();

    std::mutex control_mutex_;
    ScoutCanClient can_client_;
    ScoutCommandCodec codec_;

    std::string can_interface_{"can0"};
    int cmd_vel_timeout_ms_{500};
    int watchdog_check_interval_ms_{100};

    bool connected_{false};
    bool transport_fault_{false};
    bool watchdog_armed_{false};
    std::string last_error_;
    std::chrono::steady_clock::time_point last_cmd_vel_time_{};

    rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_sub_;
    rclcpp::TimerBase::SharedPtr watchdog_timer_;
};

}  // namespace adapter_scout
