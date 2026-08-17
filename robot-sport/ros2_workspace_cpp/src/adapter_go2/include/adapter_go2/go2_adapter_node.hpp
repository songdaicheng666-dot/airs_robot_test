#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <geometry_msgs/msg/twist.hpp>
#include <robot_adapter_interfaces/adapter_node_base.hpp>

#include "adapter_go2/go2_sdk_client.hpp"

namespace adapter_go2 {

class Go2AdapterNode : public robot_adapter_interfaces::AdapterNodeBase {
public:
    Go2AdapterNode();

protected:
    void OnConnect(TriggerResponse response) override;
    void OnDisconnect(TriggerResponse response) override;
    void OnSafeStop(TriggerResponse response) override;
    void OnHealth(TriggerResponse response) override;
    void OnSystemInfo(TriggerResponse response) override;
    void RegisterExtensions() override;

private:
    enum class ControlState {
        kDisconnected,
        kConnectedIdle,
        kConnectedCommanding,
        kFault,
    };

    struct SportStateSnapshot {
        std::array<float, 3> velocity{};
        uint32_t error_code{0};
        int mode{0};
        int gait_type{0};
    };

    struct LowStateSnapshot {
        int battery_soc{0};
        int32_t battery_current{0};
        uint16_t battery_cycle{0};
        int battery_status{0};
        int battery_version_high{0};
        int battery_version_low{0};
        std::array<int, 2> battery_bq_ntc{};
        std::array<int, 2> battery_mcu_ntc{};
        float power_v{0.0f};
        float power_a{0.0f};
    };

    void OnStandUp(TriggerResponse response);
    void OnStop(TriggerResponse response);
    void OnDamp(TriggerResponse response);
    void OnSitDown(TriggerResponse response);
    void OnEmergencyStop(TriggerResponse response);

    void OnCmdVel(const geometry_msgs::msg::Twist::SharedPtr msg);
    void OnWatchdogTick();

    template <typename Func>
    void ExecuteSportCommand(
        const std::string& command_name,
        Func&& sdk_call,
        TriggerResponse response);

    void OnSportState(const unitree_go::msg::dds_::SportModeState_& state);
    void OnLowState(const unitree_go::msg::dds_::LowState_& state);

    // Helper methods to reduce code duplication
    bool EnsureSdkInitialized(TriggerResponse response);
    bool IsConnectedSnapshot() const;
    ControlState GetControlStateSnapshot() const;

    double sdk_timeout_sec_{10.0};
    bool auto_stand_on_connect_{true};
    bool stand_down_on_disconnect_{false};
    double max_linear_x_{1.5};
    double max_linear_y_{1.0};
    double max_angular_z_{2.0};
    int cmd_vel_timeout_ms_{500};
    int watchdog_check_interval_ms_{100};
    std::string safe_stop_action_{"stop_move"};
    int connect_state_timeout_ms_{1000};
    int state_stale_timeout_ms_{2000};
    std::string network_interface_;

    std::mutex sdk_mutex_;
    mutable std::mutex node_state_mutex_;
    mutable std::mutex state_mutex_;
    Go2SdkClient sdk_;

    bool connected_{false};
    ControlState control_state_{ControlState::kDisconnected};
    std::string last_error_;
    std::chrono::steady_clock::time_point last_cmd_vel_time_{};

    SportStateSnapshot latest_sport_state_;
    LowStateSnapshot latest_low_state_;
    bool has_sport_state_{false};
    bool has_low_state_{false};
    std::chrono::steady_clock::time_point last_sport_state_time_{};
    std::chrono::steady_clock::time_point last_low_state_time_{};

    rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr cmd_vel_sub_;
    rclcpp::TimerBase::SharedPtr watchdog_timer_;

    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stand_up_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr emergency_stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr sit_down_srv_;
};

}  // namespace adapter_go2
