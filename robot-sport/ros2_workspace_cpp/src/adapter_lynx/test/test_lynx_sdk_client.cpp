#include "adapter_lynx/lynx_sdk_client.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace adapter_lynx {
namespace {

constexpr std::size_t kHeaderSize = 16;

class ScopedFd {
public:
    explicit ScopedFd(int fd) : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    [[nodiscard]] int get() const { return fd_; }

private:
    int fd_;
};

int CreateUdpServer(uint16_t* port) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    const timeval timeout{0, 500000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        close(fd);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return -1;
    }

    socklen_t address_size = sizeof(address);
    if (getsockname(
            fd, reinterpret_cast<sockaddr*>(&address), &address_size) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return fd;
}

std::optional<nlohmann::json> ReceiveCommand(
    int fd, int expected_command, sockaddr_in* client_address) {
    std::array<uint8_t, 8192> buffer{};
    for (int attempt = 0; attempt < 20; ++attempt) {
        socklen_t address_size = sizeof(*client_address);
        const auto received = recvfrom(
            fd, buffer.data(), buffer.size(), 0,
            reinterpret_cast<sockaddr*>(client_address), &address_size);
        if (received <= static_cast<ssize_t>(kHeaderSize)) {
            return std::nullopt;
        }

        const auto payload_size = static_cast<uint16_t>(buffer[4]) |
            (static_cast<uint16_t>(buffer[5]) << 8U);
        if (kHeaderSize + payload_size > static_cast<std::size_t>(received)) {
            continue;
        }

        const std::string payload(
            reinterpret_cast<const char*>(buffer.data() + kHeaderSize),
            payload_size);
        const auto json = nlohmann::json::parse(payload, nullptr, false);
        if (json.is_discarded()) {
            continue;
        }
        const auto& patrol_device = json.at("PatrolDevice");
        if (patrol_device.value("Command", -1) != expected_command) {
            continue;
        }
        return std::optional<nlohmann::json>(std::in_place, patrol_device);
    }
    return std::nullopt;
}

std::vector<uint8_t> BuildBasicStatusPacket(
    int gait,
    int motion_state = LynxSdkClient::kMotionStateRlControl,
    int usage_mode = LynxSdkClient::kUsageModeNavigation) {
    const nlohmann::json json = {
        {"PatrolDevice",
         {{"Items",
           {{"BasicStatus",
             {{"MotionState", motion_state},
              {"Gait", gait},
              {"ControlUsageMode", usage_mode}}}}}}},
    };
    const auto payload = json.dump();
    std::vector<uint8_t> packet(kHeaderSize + payload.size(), 0);
    packet[0] = 0xEB;
    packet[1] = 0x91;
    packet[2] = 0xEB;
    packet[3] = 0x90;
    const auto payload_size = static_cast<uint16_t>(payload.size());
    packet[4] = static_cast<uint8_t>(payload_size & 0xFFU);
    packet[5] = static_cast<uint8_t>((payload_size >> 8U) & 0xFFU);
    packet[8] = 0x01;
    std::memcpy(packet.data() + kHeaderSize, payload.data(), payload.size());
    return packet;
}

class LynxSdkClientGaitTest : public testing::TestWithParam<int> {};

TEST_P(LynxSdkClientGaitTest, SendsAndConfirmsAgileGait) {
    uint16_t server_port = 0;
    ScopedFd server(CreateUdpServer(&server_port));
    ASSERT_GE(server.get(), 0);

    LynxConfig config;
    config.robot_ip = "127.0.0.1";
    config.robot_port = server_port;
    config.heartbeat_interval_sec = 0.01;
    config.recv_timeout_sec = 0.01;

    LynxSdkClient client;
    std::string error;
    ASSERT_TRUE(client.Initialize(config, &error)) << error;

    const int expected_gait = GetParam();
    const auto old_version = client.GetBasicStatusVersion();
    ASSERT_TRUE(client.SetGait(expected_gait));

    sockaddr_in client_address{};
    const auto command = ReceiveCommand(server.get(), 23, &client_address);
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->at("Items").value("GaitParam", -1), expected_gait);

    const auto status_packet = BuildBasicStatusPacket(expected_gait);
    ASSERT_EQ(
        sendto(
            server.get(), status_packet.data(), status_packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&client_address),
            sizeof(client_address)),
        static_cast<ssize_t>(status_packet.size()));

    EXPECT_TRUE(client.WaitForGait(
        old_version, expected_gait, std::chrono::milliseconds(500)));
    EXPECT_TRUE(client.WaitForUsageMode(
        old_version, LynxSdkClient::kUsageModeNavigation,
        std::chrono::milliseconds(20)));
    EXPECT_TRUE(client.WaitForMotionState(
        old_version, LynxSdkClient::kMotionStateRlControl,
        std::chrono::milliseconds(20)));
    const int other_gait =
        expected_gait == LynxSdkClient::kGaitAgileFlat
        ? LynxSdkClient::kGaitAgileStairs
        : LynxSdkClient::kGaitAgileFlat;
    EXPECT_FALSE(client.WaitForGait(
        old_version, other_gait, std::chrono::milliseconds(20)));
}

TEST(LynxSdkClientVelocityTest, SendsAbsoluteVelocityWithCommand25) {
    uint16_t server_port = 0;
    ScopedFd server(CreateUdpServer(&server_port));
    ASSERT_GE(server.get(), 0);

    LynxConfig config;
    config.robot_ip = "127.0.0.1";
    config.robot_port = server_port;
    config.heartbeat_interval_sec = 0.01;
    config.recv_timeout_sec = 0.01;

    LynxSdkClient client;
    std::string error;
    ASSERT_TRUE(client.Initialize(config, &error)) << error;
    ASSERT_TRUE(client.SendVelocityCmd(0.7, -0.4, 1.2));

    sockaddr_in client_address{};
    const auto command = ReceiveCommand(server.get(), 25, &client_address);
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->value("Type", -1), 2);
    const auto& items = command->at("Items");
    EXPECT_DOUBLE_EQ(items.value("X", 0.0), 0.7);
    EXPECT_DOUBLE_EQ(items.value("Y", 0.0), -0.4);
    EXPECT_DOUBLE_EQ(items.value("Z", 1.0), 0.0);
    EXPECT_DOUBLE_EQ(items.value("Roll", 1.0), 0.0);
    EXPECT_DOUBLE_EQ(items.value("Pitch", 1.0), 0.0);
    EXPECT_DOUBLE_EQ(items.value("Yaw", 0.0), 1.2);
}

TEST(LynxSdkClientVelocityTest, PreservesLegacyAxisRatioCommand21) {
    uint16_t server_port = 0;
    ScopedFd server(CreateUdpServer(&server_port));
    ASSERT_GE(server.get(), 0);

    LynxConfig config;
    config.robot_ip = "127.0.0.1";
    config.robot_port = server_port;
    config.heartbeat_interval_sec = 0.01;
    config.recv_timeout_sec = 0.01;

    LynxSdkClient client;
    std::string error;
    ASSERT_TRUE(client.Initialize(config, &error)) << error;
    ASSERT_TRUE(client.SendMotionCmd(0.35, -0.2, 0.6));

    sockaddr_in client_address{};
    const auto command = ReceiveCommand(server.get(), 21, &client_address);
    ASSERT_TRUE(command.has_value());
    const auto& items = command->at("Items");
    EXPECT_DOUBLE_EQ(items.value("X", 0.0), 0.35);
    EXPECT_DOUBLE_EQ(items.value("Y", 0.0), -0.2);
    EXPECT_DOUBLE_EQ(items.value("Yaw", 0.0), 0.6);
}

TEST(LynxSdkClientControlTest, SendsNavigationModeAndRlControlCommands) {
    uint16_t server_port = 0;
    ScopedFd server(CreateUdpServer(&server_port));
    ASSERT_GE(server.get(), 0);

    LynxConfig config;
    config.robot_ip = "127.0.0.1";
    config.robot_port = server_port;
    config.heartbeat_interval_sec = 0.01;
    config.recv_timeout_sec = 0.01;

    LynxSdkClient client;
    std::string error;
    ASSERT_TRUE(client.Initialize(config, &error)) << error;

    sockaddr_in client_address{};
    ASSERT_TRUE(client.SetMode(LynxSdkClient::kUsageModeNavigation));
    const auto mode_command = ReceiveCommand(server.get(), 5, &client_address);
    ASSERT_TRUE(mode_command.has_value());
    EXPECT_EQ(
        mode_command->at("Items").value("Mode", -1),
        LynxSdkClient::kUsageModeNavigation);

    ASSERT_TRUE(client.SetMotionState(LynxSdkClient::kMotionStateRlControl));
    const auto motion_command = ReceiveCommand(server.get(), 22, &client_address);
    ASSERT_TRUE(motion_command.has_value());
    EXPECT_EQ(
        motion_command->at("Items").value("MotionParam", -1),
        LynxSdkClient::kMotionStateRlControl);
}

TEST(LynxSdkClientControlTest, CompletesNavigationRlAndAgileGaitSequence) {
    uint16_t server_port = 0;
    ScopedFd server(CreateUdpServer(&server_port));
    ASSERT_GE(server.get(), 0);

    LynxConfig config;
    config.robot_ip = "127.0.0.1";
    config.robot_port = server_port;
    config.heartbeat_interval_sec = 0.01;
    config.recv_timeout_sec = 0.01;

    LynxSdkClient client;
    std::string error;
    ASSERT_TRUE(client.Initialize(config, &error)) << error;

    sockaddr_in client_address{};
    auto old_version = client.GetBasicStatusVersion();
    ASSERT_TRUE(client.SetMode(LynxSdkClient::kUsageModeNavigation));
    ASSERT_TRUE(ReceiveCommand(server.get(), 5, &client_address).has_value());
    auto status_packet = BuildBasicStatusPacket(
        LynxSdkClient::kGaitStandardFlat,
        LynxSdkClient::kMotionStateStanding,
        LynxSdkClient::kUsageModeNavigation);
    ASSERT_EQ(
        sendto(
            server.get(), status_packet.data(), status_packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&client_address),
            sizeof(client_address)),
        static_cast<ssize_t>(status_packet.size()));
    ASSERT_TRUE(client.WaitForUsageMode(
        old_version, LynxSdkClient::kUsageModeNavigation,
        std::chrono::milliseconds(500)));

    old_version = client.GetBasicStatusVersion();
    ASSERT_TRUE(client.SetMotionState(LynxSdkClient::kMotionStateRlControl));
    ASSERT_TRUE(ReceiveCommand(server.get(), 22, &client_address).has_value());
    status_packet = BuildBasicStatusPacket(
        LynxSdkClient::kGaitStandardFlat,
        LynxSdkClient::kMotionStateRlControl,
        LynxSdkClient::kUsageModeNavigation);
    ASSERT_EQ(
        sendto(
            server.get(), status_packet.data(), status_packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&client_address),
            sizeof(client_address)),
        static_cast<ssize_t>(status_packet.size()));
    ASSERT_TRUE(client.WaitForMotionState(
        old_version, LynxSdkClient::kMotionStateRlControl,
        std::chrono::milliseconds(500)));

    old_version = client.GetBasicStatusVersion();
    ASSERT_TRUE(client.SetGait(LynxSdkClient::kGaitAgileFlat));
    ASSERT_TRUE(ReceiveCommand(server.get(), 23, &client_address).has_value());
    status_packet = BuildBasicStatusPacket(
        LynxSdkClient::kGaitAgileFlat,
        LynxSdkClient::kMotionStateRlControl,
        LynxSdkClient::kUsageModeNavigation);
    ASSERT_EQ(
        sendto(
            server.get(), status_packet.data(), status_packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&client_address),
            sizeof(client_address)),
        static_cast<ssize_t>(status_packet.size()));
    EXPECT_TRUE(client.WaitForGait(
        old_version, LynxSdkClient::kGaitAgileFlat,
        std::chrono::milliseconds(500)));
}

INSTANTIATE_TEST_SUITE_P(
    AgileGaits,
    LynxSdkClientGaitTest,
    testing::Values(
        LynxSdkClient::kGaitAgileFlat,
        LynxSdkClient::kGaitAgileStairs));

}  // namespace
}  // namespace adapter_lynx
