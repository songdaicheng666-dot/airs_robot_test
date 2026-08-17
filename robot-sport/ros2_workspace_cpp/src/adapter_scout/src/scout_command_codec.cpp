#include "adapter_scout/scout_command_codec.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace adapter_scout {

namespace {

void SetError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

void ValidateLimit(double value, double protocol_max, const char* name) {
    if (!std::isfinite(value) || value <= 0.0 || value > protocol_max) {
        throw std::invalid_argument(
            std::string(name) + " must be finite, positive, and no greater than " +
            std::to_string(protocol_max));
    }
}

}  // namespace

ScoutCommandCodec::ScoutCommandCodec(
    double max_linear_x_mps,
    double max_angular_z_radps)
    : max_linear_x_mps_(max_linear_x_mps)
    , max_angular_z_radps_(max_angular_z_radps) {
    ValidateLimit(
        max_linear_x_mps_, kProtocolMaxLinearMps, "max_linear_x_mps");
    ValidateLimit(
        max_angular_z_radps_, kProtocolMaxAngularRadps,
        "max_angular_z_radps");
}

std::optional<ScoutEncodedCommand> ScoutCommandCodec::EncodeVelocity(
    double linear_x_mps,
    double angular_z_radps,
    std::string* error) const {
    if (!std::isfinite(linear_x_mps) || !std::isfinite(angular_z_radps)) {
        SetError(error, "velocity contains NaN or infinity");
        return std::nullopt;
    }

    const double limited_linear = std::clamp(
        linear_x_mps, -max_linear_x_mps_, max_linear_x_mps_);
    const double limited_angular = std::clamp(
        angular_z_radps, -max_angular_z_radps_, max_angular_z_radps_);

    const auto linear_raw = static_cast<int16_t>(limited_linear * kLinearScale);
    const auto angular_raw = static_cast<int16_t>(limited_angular * kAngularScale);
    if (linear_raw < -kMaxLinearRaw || linear_raw > kMaxLinearRaw ||
        angular_raw < -kMaxAngularRaw || angular_raw > kMaxAngularRaw) {
        SetError(error, "encoded velocity exceeds Scout protocol limits");
        return std::nullopt;
    }

    if (error != nullptr) {
        error->clear();
    }
    return ScoutEncodedCommand{
        BuildVelocityFrame(linear_raw, angular_raw),
        linear_raw,
        angular_raw,
        limited_linear != linear_x_mps || limited_angular != angular_z_radps,
    };
}

ScoutCanFrame ScoutCommandCodec::BuildZeroVelocityFrame() const {
    return BuildVelocityFrame(0, 0);
}

ScoutCanFrame ScoutCommandCodec::BuildCanModeFrame() {
    ScoutCanFrame frame;
    frame.can_id = kCanModeCanId;
    frame.can_dlc = 1;
    frame.data[0] = 0x01;
    return frame;
}

ScoutCanFrame ScoutCommandCodec::BuildVelocityFrame(
    int16_t linear_raw,
    int16_t angular_raw) {
    ScoutCanFrame frame;
    frame.can_id = kControlCanId;
    frame.can_dlc = 8;

    const auto linear_bits = static_cast<uint16_t>(linear_raw);
    const auto angular_bits = static_cast<uint16_t>(angular_raw);
    frame.data[0] = static_cast<uint8_t>((linear_bits >> 8U) & 0xFFU);
    frame.data[1] = static_cast<uint8_t>(linear_bits & 0xFFU);
    frame.data[2] = static_cast<uint8_t>((angular_bits >> 8U) & 0xFFU);
    frame.data[3] = static_cast<uint8_t>(angular_bits & 0xFFU);
    return frame;
}

}  // namespace adapter_scout
