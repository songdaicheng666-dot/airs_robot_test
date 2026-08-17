#include "adapter_lynx/lynx_velocity_converter.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace adapter_lynx {
namespace {

void ValidateAxisConfig(
    const char* axis_name, double max_value, double full_scale_value) {
    if (!std::isfinite(max_value) || max_value < 0.0) {
        throw std::invalid_argument(
            std::string(axis_name) + " velocity limit must be finite and non-negative");
    }
    if (!std::isfinite(full_scale_value) || full_scale_value <= 0.0) {
        throw std::invalid_argument(
            std::string(axis_name) + " full-scale velocity must be finite and positive");
    }
    if (max_value > full_scale_value) {
        throw std::invalid_argument(
            std::string(axis_name) + " velocity limit must not exceed full-scale velocity");
    }
}

double ConvertAxis(double value, double max_value, double full_scale_value) noexcept {
    const double limited_value = std::clamp(value, -max_value, max_value);
    return std::clamp(limited_value / full_scale_value, -1.0, 1.0);
}

double LimitAxis(double value, double max_value) noexcept {
    return std::clamp(value, -max_value, max_value);
}

}  // namespace

LynxVelocityConverter::LynxVelocityConverter(LynxVelocityConversionConfig config)
    : config_(config) {
    ValidateAxisConfig("linear_x", config_.max_linear_x_mps,
                       config_.full_scale_linear_x_mps);
    ValidateAxisConfig("linear_y", config_.max_linear_y_mps,
                       config_.full_scale_linear_y_mps);
    ValidateAxisConfig("angular_z", config_.max_angular_z_radps,
                       config_.full_scale_angular_z_radps);
}

std::optional<LynxAxisCommand> LynxVelocityConverter::Convert(
    double linear_x_mps,
    double linear_y_mps,
    double angular_z_radps) const noexcept {
    const auto limited = Limit(linear_x_mps, linear_y_mps, angular_z_radps);
    if (!limited.has_value()) {
        return std::nullopt;
    }

    return LynxAxisCommand{
        ConvertAxis(limited->linear_x_mps, config_.max_linear_x_mps,
                    config_.full_scale_linear_x_mps),
        ConvertAxis(limited->linear_y_mps, config_.max_linear_y_mps,
                    config_.full_scale_linear_y_mps),
        ConvertAxis(limited->angular_z_radps, config_.max_angular_z_radps,
                    config_.full_scale_angular_z_radps),
    };
}

std::optional<LynxSiVelocityCommand> LynxVelocityConverter::Limit(
    double linear_x_mps,
    double linear_y_mps,
    double angular_z_radps) const noexcept {
    if (!std::isfinite(linear_x_mps) ||
        !std::isfinite(linear_y_mps) ||
        !std::isfinite(angular_z_radps)) {
        return std::nullopt;
    }

    return LynxSiVelocityCommand{
        LimitAxis(linear_x_mps, config_.max_linear_x_mps),
        LimitAxis(linear_y_mps, config_.max_linear_y_mps),
        LimitAxis(angular_z_radps, config_.max_angular_z_radps),
    };
}

}  // namespace adapter_lynx
