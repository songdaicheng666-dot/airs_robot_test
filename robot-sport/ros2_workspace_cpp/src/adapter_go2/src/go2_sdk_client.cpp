#include "adapter_go2/go2_sdk_client.hpp"

#include <utility>

#include <unitree/robot/channel/channel_factory.hpp>

namespace adapter_go2 {

namespace {

constexpr const char* kSportStateTopic = "rt/sportmodestate";
constexpr const char* kLowStateTopic = "rt/lowstate";

}  // namespace

bool Go2SdkClient::Initialize(const Go2SdkConfig& config, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
        return true;
    }

    try {
        unitree::robot::ChannelFactory::Instance()->Init(0, config.network_interface);

        sport_client_ = std::make_unique<unitree::robot::go2::SportClient>();
        sport_client_->SetTimeout(static_cast<float>(config.sdk_timeout_sec));
        sport_client_->Init();

        robot_state_client_ = std::make_unique<unitree::robot::go2::RobotStateClient>();
        robot_state_client_->SetTimeout(static_cast<float>(config.sdk_timeout_sec));
        robot_state_client_->Init();

        sport_state_sub_.reset(new unitree::robot::ChannelSubscriber<
                               unitree_go::msg::dds_::SportModeState_>(kSportStateTopic));
        sport_state_sub_->InitChannel(
            std::bind(&Go2SdkClient::OnSportStateRaw, this, std::placeholders::_1),
            1);

        low_state_sub_.reset(new unitree::robot::ChannelSubscriber<
                             unitree_go::msg::dds_::LowState_>(kLowStateTopic));
        low_state_sub_->InitChannel(
            std::bind(&Go2SdkClient::OnLowStateRaw, this, std::placeholders::_1),
            1);

        initialized_ = true;
        return true;
    } catch (const std::exception& e) {
        if (error != nullptr) {
            *error = std::string("SDK init failed: ") + e.what();
        }
        return false;
    }
}

bool Go2SdkClient::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

int32_t Go2SdkClient::RecoveryStand() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) {
        return -1;
    }
    return sport_client_->RecoveryStand();
}

int32_t Go2SdkClient::StopMove() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) {
        return -1;
    }
    return sport_client_->StopMove();
}

int32_t Go2SdkClient::StandDown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) {
        return -1;
    }
    return sport_client_->StandDown();
}

int32_t Go2SdkClient::Damp() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) {
        return -1;
    }
    return sport_client_->Damp();
}

int32_t Go2SdkClient::Move(float vx, float vy, float wz) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) {
        return -1;
    }
    return sport_client_->Move(vx, vy, wz);
}

int32_t
Go2SdkClient::GetServiceList(std::vector<unitree::robot::go2::ServiceState>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!robot_state_client_) {
        return -1;
    }
    return robot_state_client_->ServiceList(out);
}

void Go2SdkClient::SetSportStateCallback(SportStateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    sport_state_callback_ = std::move(callback);
}

void Go2SdkClient::SetLowStateCallback(LowStateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    low_state_callback_ = std::move(callback);
}

void Go2SdkClient::OnSportStateRaw(const void* message) {
    if (message == nullptr) {
        return;
    }

    SportStateCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = sport_state_callback_;
    }

    if (!callback) {
        return;
    }
    callback(*static_cast<const unitree_go::msg::dds_::SportModeState_*>(message));
}

void Go2SdkClient::OnLowStateRaw(const void* message) {
    if (message == nullptr) {
        return;
    }

    LowStateCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = low_state_callback_;
    }

    if (!callback) {
        return;
    }
    callback(*static_cast<const unitree_go::msg::dds_::LowState_*>(message));
}

}  // namespace adapter_go2
