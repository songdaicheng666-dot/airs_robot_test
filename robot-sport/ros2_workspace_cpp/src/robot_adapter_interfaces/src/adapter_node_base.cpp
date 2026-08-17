#include "robot_adapter_interfaces/adapter_node_base.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <rcl_yaml_param_parser/parser.h>

namespace {

std::string ReadDeviceSN(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(f)), {});
    const std::string key = "\"SN\"";
    size_t key_pos = content.find(key);
    if (key_pos == std::string::npos) return "";
    size_t colon = content.find(':', key_pos + key.size());
    if (colon == std::string::npos) return "";
    size_t open_q = content.find('"', colon + 1);
    if (open_q == std::string::npos) return "";
    size_t close_q = content.find('"', open_q + 1);
    if (close_q == std::string::npos) return "";
    return content.substr(open_q + 1, close_q - open_q - 1);
}

}  // namespace

namespace robot_adapter_interfaces {

AdapterNodeBase::AdapterNodeBase(
    const std::string& adapter_type,
    const std::string& package_name)
    : Node("adapter_" + adapter_type)
    , adapter_type_(adapter_type)
    , package_name_(
          package_name.empty() ? "adapter_" + adapter_type : package_name) {
    LoadConfigFromFile();

    const std::string prefix = "/" + std::string(get_name()) + "/";

    connect_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "connect",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnConnect(response); });

    disconnect_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "disconnect",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnDisconnect(response); });

    safe_stop_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "safe_stop",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnSafeStop(response); });

    health_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "health",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnHealth(response); });

    system_info_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "system_info",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse response) { OnSystemInfo(response); });

    hub_id_ = ReadDeviceSN("/workspace/.info/device_info.json");
    if (hub_id_.empty()) {
        RCLCPP_WARN(
            get_logger(),
            "SN not found in device_info.json; cmd_vel topic falls back to /%s/cmd_vel",
            get_name());
    } else {
        RCLCPP_INFO(get_logger(), "cmd_vel topic: /%s/cmd_vel", hub_id_.c_str());
    }
}

void AdapterNodeBase::Init() {
    if (initialized_) {
        return;
    }
    initialized_ = true;
    RegisterExtensions();
}

std::string AdapterNodeBase::GetCmdVelTopic() const {
    const std::string prefix = hub_id_.empty() ? get_name() : hub_id_;
    return "/" + prefix + "/cmd_vel";
}

void AdapterNodeBase::LoadConfigFromFile() {
    try {
        const std::string share_dir =
            ament_index_cpp::get_package_share_directory(package_name_);
        const std::string config_path =
            share_dir + "/config/" + std::string(get_name()) + ".yaml";

        if (!std::filesystem::exists(config_path)) {
            RCLCPP_INFO(
                get_logger(),
                "Config not found at %s, using defaults",
                config_path.c_str());
            return;
        }

        rcl_params_t* params =
            rcl_yaml_node_struct_init(rcutils_get_default_allocator());
        if (params == nullptr) {
            RCLCPP_WARN(get_logger(), "Failed to init YAML parser");
            return;
        }

        if (!rcl_parse_yaml_file(config_path.c_str(), params)) {
            RCLCPP_WARN(
                get_logger(),
                "Failed to parse YAML file: %s",
                config_path.c_str());
            rcl_yaml_node_struct_fini(params);
            return;
        }

        for (size_t node_index = 0; node_index < params->num_nodes;
             ++node_index) {
            const char* node_name = params->node_names[node_index];
            if (node_name == nullptr) {
                continue;
            }

            std::string normalized_node_name(node_name);
            const size_t slash_index = normalized_node_name.find_last_of('/');
            if (slash_index != std::string::npos) {
                normalized_node_name = normalized_node_name.substr(slash_index + 1);
            }

            if (normalized_node_name != get_name()) {
                continue;
            }

            const rcl_node_params_t* node_params = &params->params[node_index];
            for (size_t param_index = 0; param_index < node_params->num_params;
                 ++param_index) {
                const char* param_name = node_params->parameter_names[param_index];
                const rcl_variant_t* param_value =
                    &node_params->parameter_values[param_index];
                if (param_name == nullptr) {
                    continue;
                }

                rclcpp::Parameter parameter;
                if (param_value->bool_value != nullptr) {
                    parameter =
                        rclcpp::Parameter(param_name, *param_value->bool_value);
                } else if (param_value->integer_value != nullptr) {
                    parameter =
                        rclcpp::Parameter(param_name, *param_value->integer_value);
                } else if (param_value->double_value != nullptr) {
                    parameter =
                        rclcpp::Parameter(param_name, *param_value->double_value);
                } else if (param_value->string_value != nullptr) {
                    parameter = rclcpp::Parameter(
                        param_name,
                        std::string(param_value->string_value));
                } else {
                    continue;
                }

                config_overrides_[param_name] = parameter;
            }
        }

        rcl_yaml_node_struct_fini(params);
    } catch (const std::exception& e) {
        RCLCPP_WARN(
            get_logger(),
            "Failed to load config file, using defaults: %s",
            e.what());
    }
}

}  // namespace robot_adapter_interfaces
