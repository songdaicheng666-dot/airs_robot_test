#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <unitree/idl/go2/LowState_.hpp>
#include <unitree/idl/go2/SportModeState_.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>
#include <unitree/robot/go2/robot_state/robot_state_client.hpp>
#include <unitree/robot/go2/sport/sport_client.hpp>

namespace adapter_go2 {

struct Go2SdkConfig {
    std::string network_interface;
    double sdk_timeout_sec{10.0};
};

class Go2SdkClient {
public:
    using SportStateCallback =
        std::function<void(const unitree_go::msg::dds_::SportModeState_&)>;
    using LowStateCallback =
        std::function<void(const unitree_go::msg::dds_::LowState_&)>;

    Go2SdkClient() = default;
    ~Go2SdkClient() = default;

    Go2SdkClient(const Go2SdkClient&) = delete;
    Go2SdkClient& operator=(const Go2SdkClient&) = delete;

    bool Initialize(const Go2SdkConfig& config, std::string* error = nullptr);
    bool IsInitialized() const;

    int32_t RecoveryStand();
    int32_t StopMove();
    int32_t StandDown();
    int32_t Damp();
    int32_t Move(float vx, float vy, float wz);
    int32_t GetServiceList(std::vector<unitree::robot::go2::ServiceState>& out);

    void SetSportStateCallback(SportStateCallback callback);
    void SetLowStateCallback(LowStateCallback callback);

private:
    void OnSportStateRaw(const void* message);
    void OnLowStateRaw(const void* message);

    mutable std::mutex mutex_;
    bool initialized_{false};

    std::unique_ptr<unitree::robot::go2::SportClient> sport_client_;
    std::unique_ptr<unitree::robot::go2::RobotStateClient> robot_state_client_;
    unitree::robot::ChannelSubscriberPtr<unitree_go::msg::dds_::SportModeState_>
        sport_state_sub_;
    unitree::robot::ChannelSubscriberPtr<unitree_go::msg::dds_::LowState_>
        low_state_sub_;

    SportStateCallback sport_state_callback_;
    LowStateCallback low_state_callback_;
};

}  // namespace adapter_go2
