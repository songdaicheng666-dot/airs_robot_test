#pragma once

#include <optional>

namespace adapter_lynx {

struct LynxVelocityConversionConfig {
    double max_linear_x_mps{1.5};
    double max_linear_y_mps{1.0};
    double max_angular_z_radps{2.0};
    double full_scale_linear_x_mps{2.0};
    double full_scale_linear_y_mps{2.0};
    double full_scale_angular_z_radps{2.0};
};

struct LynxAxisCommand {
    double x_ratio{0.0};
    double y_ratio{0.0};
    double yaw_ratio{0.0};
};

struct LynxSiVelocityCommand {
    double linear_x_mps{0.0};
    double linear_y_mps{0.0};
    double angular_z_radps{0.0};
};

// Converts ROS cmd_vel values in SI units to Lynx Command=21 axis ratios.
class LynxVelocityConverter {
public:
    explicit LynxVelocityConverter(
        LynxVelocityConversionConfig config = LynxVelocityConversionConfig{});

    // Returns nullopt for non-finite input. Output axes are always in [-1, 1].
    [[nodiscard]] std::optional<LynxAxisCommand> Convert(
        double linear_x_mps,
        double linear_y_mps,
        double angular_z_radps) const noexcept;

    // Applies the same SI safety limits without normalizing. The result is
    // suitable for Lynx Command=25 in navigation mode.
    [[nodiscard]] std::optional<LynxSiVelocityCommand> Limit(
        double linear_x_mps,
        double linear_y_mps,
        double angular_z_radps) const noexcept;

private:
    LynxVelocityConversionConfig config_;
};

}  // namespace adapter_lynx
