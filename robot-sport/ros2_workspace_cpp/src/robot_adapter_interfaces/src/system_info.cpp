#include "robot_adapter_interfaces/system_info.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <unordered_set>
#include <utility>

namespace robot_adapter_interfaces {

namespace {

nlohmann::json ParseDetailsObject(const std::string& details_json) {
    if (details_json.empty()) {
        return nlohmann::json::object();
    }

    const auto parsed = nlohmann::json::parse(details_json, nullptr, false);
    if (parsed.is_discarded()) {
        return nlohmann::json{{"_raw", details_json}};
    }

    if (parsed.is_object()) {
        return parsed;
    }

    return nlohmann::json{{"_raw", parsed}};
}

nlohmann::json ToBatteryJson(const SystemInfoBuilder::BatteryValue& battery) {
    if (std::holds_alternative<int>(battery)) {
        return nlohmann::json(std::get<int>(battery));
    }
    return nlohmann::json(std::get<std::vector<int>>(battery));
}

nlohmann::json ToMotionJson(const MotionState& motion) {
    return nlohmann::json{
        {"x", motion.x},
        {"y", motion.y},
        {"yaw", motion.yaw},
    };
}

nlohmann::json ToMotionsJson(const std::vector<MotionDescriptor>& motions) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : motions) {
        arr.push_back({
            {"id", m.id},
            {"service_suffix", m.service_suffix},
            {"description", m.description},
            {"display_name", m.display_name},
        });
    }
    return arr;
}

}  // namespace

SystemInfoBuilder& SystemInfoBuilder::SetBattery(int battery_percentage) {
    battery_ = battery_percentage;
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetBattery(
    std::vector<int> battery_percentages) {
    battery_ = std::move(battery_percentages);
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetMotion(double x, double y, double yaw) {
    motion_ = MotionState{x, y, yaw};
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetMotion(const MotionState& motion) {
    motion_ = motion;
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetDetailsJson(std::string details_json) {
    details_json_ = std::move(details_json);
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetMotions(
    std::vector<MotionDescriptor> motions) {
    std::vector<MotionDescriptor> valid;
    valid.reserve(motions.size());
    std::unordered_set<std::string> seen;
    for (auto& m : motions) {
        if (!IsValidMotionId(m.id)) {
            std::cerr << "[SystemInfoBuilder] dropping motion: invalid id '"
                      << m.id << "'" << std::endl;
            continue;
        }
        if (m.service_suffix.empty()) {
            std::cerr << "[SystemInfoBuilder] dropping motion '" << m.id
                      << "': empty service_suffix" << std::endl;
            continue;
        }
        if (!seen.insert(m.id).second) {
            std::cerr << "[SystemInfoBuilder] dropping motion '" << m.id
                      << "': duplicate id" << std::endl;
            continue;
        }
        // 短名缺失只降级,不丢动作:前端拿到 id 总好过拿到空按钮。
        if (m.display_name.empty()) {
            m.display_name = m.id;
        }
        valid.push_back(std::move(m));
    }
    motions_ = std::move(valid);
    return *this;
}

std::string SystemInfoBuilder::Build() const {
    nlohmann::json result = nlohmann::json::object();

    if (battery_.has_value()) {
        result["battery"] = ToBatteryJson(*battery_);
    } else {
        result["battery"] = nullptr;
    }

    if (motion_.has_value()) {
        result["motion"] = ToMotionJson(*motion_);
    } else {
        result["motion"] = nullptr;
    }

    // Absent (not null) when SetMotions was never called — keeps
    // adapters that don't declare motions byte-identical to pre-patch builds.
    if (motions_.has_value()) {
        result["motions"] = ToMotionsJson(*motions_);
    }

    result["details"] = ParseDetailsObject(details_json_);
    return result.dump();
}

}  // namespace robot_adapter_interfaces
