#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace adapter_scout {

struct ScoutCanFrame {
    uint32_t can_id{0};
    uint8_t can_dlc{0};
    std::array<uint8_t, 8> data{};
};

struct ScoutEncodedCommand {
    ScoutCanFrame frame;
    int16_t linear_raw{0};
    int16_t angular_raw{0};
    bool limited{false};

    [[nodiscard]] bool IsZero() const {
        return linear_raw == 0 && angular_raw == 0;
    }
};

class ScoutCommandCodec {
public:
    static constexpr uint32_t kControlCanId = 0x111;
    static constexpr uint32_t kCanModeCanId = 0x421;
    static constexpr int16_t kMaxLinearRaw = 1500;
    static constexpr int16_t kMaxAngularRaw = 523;
    static constexpr double kLinearScale = 1000.0;
    static constexpr double kAngularScale = 400.0;
    static constexpr double kProtocolMaxLinearMps =
        static_cast<double>(kMaxLinearRaw) / kLinearScale;
    static constexpr double kProtocolMaxAngularRadps =
        static_cast<double>(kMaxAngularRaw) / kAngularScale;

    ScoutCommandCodec(
        double max_linear_x_mps = kProtocolMaxLinearMps,
        double max_angular_z_radps = kProtocolMaxAngularRadps);

    [[nodiscard]] std::optional<ScoutEncodedCommand> EncodeVelocity(
        double linear_x_mps,
        double angular_z_radps,
        std::string* error = nullptr) const;

    [[nodiscard]] ScoutCanFrame BuildZeroVelocityFrame() const;
    [[nodiscard]] static ScoutCanFrame BuildCanModeFrame();

private:
    [[nodiscard]] static ScoutCanFrame BuildVelocityFrame(
        int16_t linear_raw,
        int16_t angular_raw);

    double max_linear_x_mps_;
    double max_angular_z_radps_;
};

}  // namespace adapter_scout
