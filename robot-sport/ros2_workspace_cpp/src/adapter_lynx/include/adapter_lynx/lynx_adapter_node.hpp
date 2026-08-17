#pragma once

#include <chrono>
#include <mutex>
#include <string>

#include <geometry_msgs/msg/twist.hpp>
#include <robot_adapter_interfaces/adapter_node_base.hpp>

#include "adapter_lynx/lynx_sdk_client.hpp"
#include "adapter_lynx/lynx_velocity_converter.hpp"

namespace adapter_lynx {

class LynxAdapterNode : public robot_adapter_interfaces::AdapterNodeBase {
public:
    LynxAdapterNode();

protected:
    // ---- 框架必须实现的 5 个纯虚函数 ----
    void OnConnect(TriggerResponse response) override;
    void OnDisconnect(TriggerResponse response) override;
    void OnSafeStop(TriggerResponse response) override;
    void OnHealth(TriggerResponse response) override;
    void OnSystemInfo(TriggerResponse response) override;

    // ---- 注册扩展 Service/Topic ----
    void RegisterExtensions() override;

private:
    // ---- 控制类扩展 Service 处理 ----
    void OnModeRegular(TriggerResponse response);      // 切常规模式
    void OnModeNavigation(TriggerResponse response);   // 切导航模式
    void SwitchUsageMode(
        int mode, const char* mode_name, TriggerResponse response);
    bool EnsureUsageModeLocked(
        int mode, const char* mode_name, std::string* error);
    void OnGaitWalk(TriggerResponse response);         // 步态:walk
    void OnGaitTrot(TriggerResponse response);         // 步态:trot
    void OnGaitStandardFlat(TriggerResponse response); // 标准平地步态
    void OnGaitStandardStairs(TriggerResponse response);// 标准楼梯步态
    void OnGaitAgileFlat(TriggerResponse response);    // 敏捷平地步态
    void OnGaitAgileStairs(TriggerResponse response);  // 敏捷楼梯步态
    void SwitchGait(
        int gait, const char* gait_name, TriggerResponse response);
    void OnStandUp(TriggerResponse response);          // 运动状态:站起
    void OnSoftStop(TriggerResponse response);         // 运动状态:软急停
    void OnSitDown(TriggerResponse response);          // 运动状态:趴下
    void OnRlControl(TriggerResponse response);        // 运动状态:RL 控制
    void SwitchMotionState(
        int motion_state,
        const char* motion_state_name,
        TriggerResponse response);
    bool EnsureMotionStateLocked(
        int motion_state,
        const char* motion_state_name,
        std::string* error);
    void OnLightsOn(TriggerResponse response);         // 照明灯全开
    void OnLightsOff(TriggerResponse response);        // 照明灯全关
    void OnChargeStart(TriggerResponse response);      // 进入自主充电
    void OnChargeStop(TriggerResponse response);       // 退出自主充电
    void OnSleepEnter(TriggerResponse response);       // 进入休眠
    void OnSleepExit(TriggerResponse response);        // 退出休眠
    void OnQuerySleepStatus(TriggerResponse response); // 查休眠设置

    // ---- Topic 回调 ----
    void OnCmdVel(const geometry_msgs::msg::Twist::SharedPtr msg);

    // ---- 看门狗 ----
    void OnWatchdogTick();

    [[nodiscard]] bool IsBasicStatusFresh(const LynxStatus& status) const;
    bool SendZeroVelocityForMode(int mode);
    bool SendZeroVelocityForCurrentMode();

    // ---- 配置参数 ----
    std::string robot_ip_{"10.21.31.103"};
    int robot_port_{30000};
    int local_port_{0};
    double heartbeat_interval_sec_{1.0};
    double recv_timeout_sec_{1.0};
    int initial_mode_{0};
    int initial_motion_state_{1};
    int cmd_vel_timeout_ms_{500};
    int watchdog_check_interval_ms_{100};
    int query_timeout_ms_{1000};
    int control_transition_timeout_ms_{1500};
    int connect_status_timeout_ms_{2000};
    int status_stale_timeout_ms_{2000};

    // ---- 状态 ----
    mutable std::mutex node_state_mutex_;
    mutable std::mutex control_mutex_;
    LynxSdkClient sdk_;
    LynxVelocityConverter velocity_converter_;
    bool connected_{false};
    std::string last_error_;
    std::chrono::steady_clock::time_point last_cmd_vel_time_{};

    // ---- ROS2 资源句柄 ----
    rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr       cmd_vel_sub_;
    rclcpp::TimerBase::SharedPtr watchdog_timer_;

    // 控制类扩展 services
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr mode_regular_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr mode_navigation_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr gait_walk_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr gait_trot_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr gait_standard_flat_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr gait_standard_stairs_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr gait_agile_flat_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr gait_agile_stairs_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stand_up_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr soft_stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr sit_down_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr rl_control_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr lights_on_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr lights_off_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr charge_start_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr charge_stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr sleep_enter_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr sleep_exit_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr sleep_query_srv_;

};

}  // namespace adapter_lynx
