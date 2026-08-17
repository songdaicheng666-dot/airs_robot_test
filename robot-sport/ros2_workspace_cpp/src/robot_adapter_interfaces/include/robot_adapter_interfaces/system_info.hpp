#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace robot_adapter_interfaces {

struct MotionState {
    double x{0.0};
    double y{0.0};
    double yaw{0.0};
};

struct MotionDescriptor {
    std::string id;              // [A-Za-z0-9_]+, non-empty
    std::string service_suffix;  // appended to adapter service_prefix (e.g. "stop_and_sit")
    std::string description;     // optional UX label; "" if none
    // 前端直接渲染的中文短名(2-4 字)。留空时 SetMotions 会回填 id,调用方永远拿不到空串。
    //
    // 必须保持在最后一个成员。所有调用点都用位置聚合初始化
    // ({"id", "suffix", "desc", "label"});把它挪到 description 之前不会编译失败,
    // 而是把英文长句静默塞进 display_name。
    std::string display_name;
};

// Motion id charset: non-empty, [A-Za-z0-9_]+. Shared canonical check so
// adapter-side SystemInfoBuilder and switch-server-side motion cache apply
// identical rules.
inline bool IsValidMotionId(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

class SystemInfoBuilder {
public:
    using BatteryValue = std::variant<int, std::vector<int>>;

    SystemInfoBuilder& SetBattery(int battery_percentage);
    SystemInfoBuilder& SetBattery(std::vector<int> battery_percentages);
    SystemInfoBuilder& SetMotion(double x, double y, double yaw);
    SystemInfoBuilder& SetMotion(const MotionState& motion);
    SystemInfoBuilder& SetDetailsJson(std::string details_json);
    SystemInfoBuilder& SetMotions(std::vector<MotionDescriptor> motions);

    [[nodiscard]] std::string Build() const;

private:
    std::optional<BatteryValue> battery_;
    std::optional<MotionState> motion_;
    std::string details_json_;
    std::optional<std::vector<MotionDescriptor>> motions_;
};

}  // namespace robot_adapter_interfaces
