#include "adapter_lynx/lynx_velocity_converter.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace adapter_lynx {
namespace {

TEST(LynxVelocityConverterTest, ConvertsConfirmedLinearSpeedToAxisRatio) {
    const LynxVelocityConverter converter;

    const auto command = converter.Convert(0.7, 0.0, 0.0);

    ASSERT_TRUE(command.has_value());
    EXPECT_NEAR(command->x_ratio, 0.35, 1e-12);
    EXPECT_DOUBLE_EQ(command->y_ratio, 0.0);
    EXPECT_DOUBLE_EQ(command->yaw_ratio, 0.0);
}

TEST(LynxVelocityConverterTest, ConvertsEachAxisUsingItsOwnFullScale) {
    const LynxVelocityConverter converter;

    const auto command = converter.Convert(1.0, -0.5, 1.0);

    ASSERT_TRUE(command.has_value());
    EXPECT_NEAR(command->x_ratio, 0.5, 1e-12);
    EXPECT_NEAR(command->y_ratio, -0.25, 1e-12);
    EXPECT_NEAR(command->yaw_ratio, 0.5, 1e-12);
}

TEST(LynxVelocityConverterTest, AppliesPhysicalSafetyLimitsBeforeConversion) {
    const LynxVelocityConverter converter;

    const auto command = converter.Convert(3.0, -3.0, 3.0);

    ASSERT_TRUE(command.has_value());
    EXPECT_NEAR(command->x_ratio, 0.75, 1e-12);
    EXPECT_NEAR(command->y_ratio, -0.5, 1e-12);
    EXPECT_NEAR(command->yaw_ratio, 1.0, 1e-12);
}

TEST(LynxVelocityConverterTest, RejectsNonFiniteInput) {
    const LynxVelocityConverter converter;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();

    EXPECT_FALSE(converter.Convert(nan, 0.0, 0.0).has_value());
    EXPECT_FALSE(converter.Convert(0.0, infinity, 0.0).has_value());
    EXPECT_FALSE(converter.Convert(0.0, 0.0, -infinity).has_value());
}

TEST(LynxVelocityConverterTest, KeepsSiUnitsForNavigationCommand) {
    const LynxVelocityConverter converter;

    const auto command = converter.Limit(0.7, -0.4, 1.2);

    ASSERT_TRUE(command.has_value());
    EXPECT_DOUBLE_EQ(command->linear_x_mps, 0.7);
    EXPECT_DOUBLE_EQ(command->linear_y_mps, -0.4);
    EXPECT_DOUBLE_EQ(command->angular_z_radps, 1.2);
}

TEST(LynxVelocityConverterTest, AppliesSameSafetyLimitsToNavigationCommand) {
    const LynxVelocityConverter converter;

    const auto command = converter.Limit(3.0, -3.0, 3.0);

    ASSERT_TRUE(command.has_value());
    EXPECT_DOUBLE_EQ(command->linear_x_mps, 1.5);
    EXPECT_DOUBLE_EQ(command->linear_y_mps, -1.0);
    EXPECT_DOUBLE_EQ(command->angular_z_radps, 2.0);
}

TEST(LynxVelocityConverterTest, RejectsNonFiniteNavigationInput) {
    const LynxVelocityConverter converter;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    EXPECT_FALSE(converter.Limit(nan, 0.0, 0.0).has_value());
}

TEST(LynxVelocityConverterTest, RejectsInvalidConfiguration) {
    LynxVelocityConversionConfig config;

    config.max_linear_x_mps = -0.1;
    EXPECT_THROW((void)LynxVelocityConverter(config), std::invalid_argument);

    config = LynxVelocityConversionConfig{};
    config.full_scale_linear_y_mps = 0.0;
    EXPECT_THROW((void)LynxVelocityConverter(config), std::invalid_argument);

    config = LynxVelocityConversionConfig{};
    config.full_scale_linear_x_mps = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW((void)LynxVelocityConverter(config), std::invalid_argument);

    config = LynxVelocityConversionConfig{};
    config.max_angular_z_radps = 2.1;
    EXPECT_THROW((void)LynxVelocityConverter(config), std::invalid_argument);
}

}  // namespace
}  // namespace adapter_lynx
