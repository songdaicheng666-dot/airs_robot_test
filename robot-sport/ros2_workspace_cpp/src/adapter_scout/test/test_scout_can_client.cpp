#include "adapter_scout/scout_can_client.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace adapter_scout {
namespace {

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

TEST(ScoutCanClientTest, RejectsSendWhileDisconnected) {
    ScoutCanClient client;
    std::string error;

    EXPECT_FALSE(client.Send(ScoutCommandCodec::BuildCanModeFrame(), &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(client.IsConnected());
}

TEST(ScoutCanClientTest, RejectsInvalidInterfaceNames) {
    ScoutCanClient client;
    std::string error;

    EXPECT_FALSE(client.Connect("", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(client.Connect(std::string(IFNAMSIZ, 'x'), &error));
    EXPECT_FALSE(error.empty());
}

TEST(ScoutCanClientTest, ReportsMissingInterfaceAndDisconnectIsIdempotent) {
    ScoutCanClient client;
    std::string error;

    EXPECT_FALSE(client.Connect("scout_missing", &error));
    EXPECT_FALSE(error.empty());
    client.Disconnect();
    client.Disconnect();
    EXPECT_FALSE(client.IsConnected());
}

TEST(ScoutCanClientTest, SendsStandardFrameThroughVcan) {
    constexpr char kInterface[] = "vcan0";
    const unsigned int interface_index = if_nametoindex(kInterface);
    if (interface_index == 0) {
        GTEST_SKIP() << "vcan0 is not available";
    }

    std::string interface_error;
    if (!ScoutCanClient::CheckInterfaceUp(kInterface, &interface_error)) {
        GTEST_SKIP() << interface_error;
    }

    ScopedFd peer(socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW));
    ASSERT_GE(peer.get(), 0) << std::strerror(errno);

    const can_filter filter{
        ScoutCommandCodec::kCanModeCanId,
        CAN_SFF_MASK,
    };
    ASSERT_EQ(
        setsockopt(
            peer.get(), SOL_CAN_RAW, CAN_RAW_FILTER, &filter, sizeof(filter)),
        0) << std::strerror(errno);

    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = static_cast<int>(interface_index);
    ASSERT_EQ(
        bind(
            peer.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0) << std::strerror(errno);

    ScoutCanClient client;
    std::string error;
    ASSERT_TRUE(client.Connect(kInterface, &error)) << error;
    EXPECT_TRUE(client.Connect(kInterface, &error)) << error;
    EXPECT_TRUE(client.IsConnected());
    EXPECT_EQ(client.interface_name(), kInterface);

    const auto sent = ScoutCommandCodec::BuildCanModeFrame();
    ASSERT_TRUE(client.Send(sent, &error)) << error;

    pollfd descriptor{peer.get(), POLLIN, 0};
    ASSERT_GT(poll(&descriptor, 1, 500), 0) << std::strerror(errno);
    ASSERT_NE(descriptor.revents & POLLIN, 0);

    can_frame received{};
    ASSERT_EQ(
        read(peer.get(), &received, sizeof(received)),
        static_cast<ssize_t>(sizeof(received))) << std::strerror(errno);
    EXPECT_EQ(received.can_id, sent.can_id);
    EXPECT_EQ(received.can_dlc, sent.can_dlc);
    EXPECT_EQ(received.data[0], sent.data[0]);

    client.Disconnect();
    EXPECT_FALSE(client.IsConnected());
}

}  // namespace
}  // namespace adapter_scout
