#include "adapter_scout/scout_command_codec.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace adapter_scout {
namespace {

void ExpectPayload(
    const ScoutCanFrame& frame,
    const std::array<uint8_t, 8>& expected) {
    EXPECT_EQ(frame.can_id, ScoutCommandCodec::kControlCanId);
    EXPECT_EQ(frame.can_dlc, 8U);
    EXPECT_EQ(frame.data, expected);
}

TEST(ScoutCommandCodecTest, EncodesZeroVelocity) {
    const ScoutCommandCodec codec;
    const auto command = codec.EncodeVelocity(0.0, 0.0);

    ASSERT_TRUE(command.has_value());
    EXPECT_TRUE(command->IsZero());
    EXPECT_FALSE(command->limited);
    ExpectPayload(command->frame, {0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0});
}

TEST(ScoutCommandCodecTest, EncodesPositiveVelocityBigEndian) {
    const ScoutCommandCodec codec;
    const auto command = codec.EncodeVelocity(1.0, 0.5);

    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->linear_raw, 1000);
    EXPECT_EQ(command->angular_raw, 200);
    ExpectPayload(command->frame, {0x03, 0xE8, 0x00, 0xC8, 0, 0, 0, 0});
}

TEST(ScoutCommandCodecTest, EncodesNegativeVelocityTwosComplement) {
    const ScoutCommandCodec codec;
    const auto command = codec.EncodeVelocity(-1.0, -0.5);

    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->linear_raw, -1000);
    EXPECT_EQ(command->angular_raw, -200);
    ExpectPayload(command->frame, {0xFC, 0x18, 0xFF, 0x38, 0, 0, 0, 0});
}

TEST(ScoutCommandCodecTest, EncodesProtocolBoundaries) {
    const ScoutCommandCodec codec;

    const auto positive = codec.EncodeVelocity(1.5, 1.3075);
    ASSERT_TRUE(positive.has_value());
    EXPECT_EQ(positive->linear_raw, ScoutCommandCodec::kMaxLinearRaw);
    EXPECT_EQ(positive->angular_raw, ScoutCommandCodec::kMaxAngularRaw);
    ExpectPayload(positive->frame, {0x05, 0xDC, 0x02, 0x0B, 0, 0, 0, 0});

    const auto negative = codec.EncodeVelocity(-1.5, -1.3075);
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(negative->linear_raw, -ScoutCommandCodec::kMaxLinearRaw);
    EXPECT_EQ(negative->angular_raw, -ScoutCommandCodec::kMaxAngularRaw);
    ExpectPayload(negative->frame, {0xFA, 0x24, 0xFD, 0xF5, 0, 0, 0, 0});
}

TEST(ScoutCommandCodecTest, ClampsFiniteValuesToConfiguredLimits) {
    const ScoutCommandCodec codec(0.5, 0.25);
    const auto command = codec.EncodeVelocity(2.0, -2.0);

    ASSERT_TRUE(command.has_value());
    EXPECT_TRUE(command->limited);
    EXPECT_EQ(command->linear_raw, 500);
    EXPECT_EQ(command->angular_raw, -100);
    ExpectPayload(command->frame, {0x01, 0xF4, 0xFF, 0x9C, 0, 0, 0, 0});
}

TEST(ScoutCommandCodecTest, PreservesLegacyTruncationTowardZero) {
    const ScoutCommandCodec codec;
    const auto command = codec.EncodeVelocity(0.0019, -0.0049);

    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->linear_raw, 1);
    EXPECT_EQ(command->angular_raw, -1);
}

TEST(ScoutCommandCodecTest, RejectsNonFiniteVelocity) {
    const ScoutCommandCodec codec;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    std::string error;

    EXPECT_FALSE(codec.EncodeVelocity(nan, 0.0, &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(codec.EncodeVelocity(0.0, infinity, &error).has_value());
    EXPECT_FALSE(codec.EncodeVelocity(-infinity, 0.0, &error).has_value());
}

TEST(ScoutCommandCodecTest, RejectsInvalidConfiguration) {
    EXPECT_THROW((void)ScoutCommandCodec(0.0, 1.0), std::invalid_argument);
    EXPECT_THROW((void)ScoutCommandCodec(1.0, -1.0), std::invalid_argument);
    EXPECT_THROW((void)ScoutCommandCodec(1.6, 1.0), std::invalid_argument);
    EXPECT_THROW((void)ScoutCommandCodec(1.0, 1.4), std::invalid_argument);
    EXPECT_THROW(
        (void)ScoutCommandCodec(
            std::numeric_limits<double>::quiet_NaN(), 1.0),
        std::invalid_argument);
}

TEST(ScoutCommandCodecTest, BuildsCanModeFrame) {
    const auto frame = ScoutCommandCodec::BuildCanModeFrame();

    EXPECT_EQ(frame.can_id, ScoutCommandCodec::kCanModeCanId);
    EXPECT_EQ(frame.can_dlc, 1U);
    EXPECT_EQ(frame.data[0], 0x01U);
    for (std::size_t index = 1; index < frame.data.size(); ++index) {
        EXPECT_EQ(frame.data[index], 0U);
    }
}

}  // namespace
}  // namespace adapter_scout
