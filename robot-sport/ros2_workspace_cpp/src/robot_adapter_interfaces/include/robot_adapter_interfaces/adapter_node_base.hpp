#pragma once

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/trigger.hpp>

namespace robot_adapter_interfaces {

class AdapterNodeBase : public rclcpp::Node {
public:
    using TriggerResponse = std::shared_ptr<std_srvs::srv::Trigger::Response>;

    explicit AdapterNodeBase(
        const std::string& adapter_type,
        const std::string& package_name = "");
    ~AdapterNodeBase() override = default;

    void Init();

protected:
    virtual void OnConnect(TriggerResponse response) = 0;
    virtual void OnDisconnect(TriggerResponse response) = 0;
    virtual void OnSafeStop(TriggerResponse response) = 0;
    virtual void OnHealth(TriggerResponse response) = 0;
    virtual void OnSystemInfo(TriggerResponse response) = 0;

    virtual void RegisterExtensions() {}
    void LoadConfigFromFile();
    std::string GetCmdVelTopic() const;

    template <typename T>
    T GetParamOrDefault(const std::string& name, const T& default_value) {
        const auto it = config_overrides_.find(name);
        if (it != config_overrides_.end()) {
            try {
                return it->second.get_value<T>();
            } catch (const std::exception& e) {
                RCLCPP_WARN(
                    get_logger(),
                    "Failed to convert param '%s': %s",
                    name.c_str(),
                    e.what());
            }
        }
        return default_value;
    }

    template <typename T>
    T GetRequiredParam(const std::string& name) {
        const auto it = config_overrides_.find(name);
        if (it != config_overrides_.end()) {
            try {
                return it->second.get_value<T>();
            } catch (const std::exception& e) {
                throw std::runtime_error(
                    "Failed to convert param '" + name + "': " + e.what());
            }
        }
        throw std::runtime_error(
            "Required parameter '" + name + "' not found in config");
    }

private:
    std::string adapter_type_;
    std::string package_name_;
    bool initialized_{false};
    std::string hub_id_;
    std::unordered_map<std::string, rclcpp::Parameter> config_overrides_;

    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr connect_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr disconnect_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr safe_stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr health_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr system_info_srv_;
};

}  // namespace robot_adapter_interfaces
