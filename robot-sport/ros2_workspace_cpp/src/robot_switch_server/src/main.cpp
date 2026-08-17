#include "robot_switch_server/core/adapter_process_supervisor.hpp"
#include "robot_switch_server/core/adapter_runtime_manager.hpp"
#include "robot_switch_server/infra/http_server_runner.hpp"

#include <chrono>
#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <vector>

namespace robot_switch_server {

namespace {

// Parameter name constants
constexpr char kHttpListenAddress[] = "http_listen_address";
constexpr char kServiceWaitMs[] = "service_wait_ms";
constexpr char kCallTimeoutMs[] = "call_timeout_ms";
constexpr char kAdapterConnectTimeoutMs[] = "adapter_connect_timeout_ms";
constexpr char kEnabledAdapterTypes[] = "enabled_adapter_types";

// Default value constants
constexpr char kDefaultListenAddress[] = "0.0.0.0:8080";
constexpr int kDefaultServiceWaitMs = 500;
constexpr int kDefaultCallTimeoutMs = 1200;
constexpr int kDefaultConnectTimeoutMs = 8000;

/**
 * @brief Application configuration parameters.
 */
struct AppConfig {
    std::string listen_address;
    int service_wait_ms = 0;
    int call_timeout_ms = 0;
    int connect_timeout_ms = 0;
    std::vector<std::string> enabled_adapter_types;
};

/**
 * @brief Loads all application parameters from the ROS2 node.
 * @param node ROS2 node to read parameters from.
 * @return Populated AppConfig structure.
 */
AppConfig LoadConfig(const rclcpp::Node::SharedPtr& node) {
    AppConfig config;
    config.listen_address =
        node->declare_parameter<std::string>(kHttpListenAddress, kDefaultListenAddress);
    config.service_wait_ms =
        node->declare_parameter<int>(kServiceWaitMs, kDefaultServiceWaitMs);
    config.call_timeout_ms =
        node->declare_parameter<int>(kCallTimeoutMs, kDefaultCallTimeoutMs);
    config.connect_timeout_ms =
        node->declare_parameter<int>(kAdapterConnectTimeoutMs, kDefaultConnectTimeoutMs);
    config.enabled_adapter_types = node->declare_parameter<std::vector<std::string>>(
        kEnabledAdapterTypes, std::vector<std::string>{"go2"});
    return config;
}

}  // namespace

}  // namespace robot_switch_server

int main(int argc, char** argv) {
    using namespace robot_switch_server;

    // Block SIGCHLD before any threads are created (inherited by all threads)
    AdapterProcessSupervisor::BlockSigchld();

    rclcpp::init(argc, argv);
    const auto node = std::make_shared<rclcpp::Node>("robot_switch_server");

    const AppConfig config = LoadConfig(node);

    // Create process supervisor
    auto supervisor = std::make_unique<AdapterProcessSupervisor>();

    const auto runtime_manager = std::make_shared<AdapterRuntimeManager>(
        node, supervisor.get(),
        std::chrono::milliseconds(config.service_wait_ms),
        std::chrono::milliseconds(config.call_timeout_ms),
        std::chrono::milliseconds(config.connect_timeout_ms),
        config.enabled_adapter_types);

    // Start supervisor with child exit callback
    if (!supervisor->Start([runtime_manager](const ProcessExitInfo& info) {
            runtime_manager->OnChildExited(info);
        })) {
        RCLCPP_ERROR(node->get_logger(), "Failed to start process supervisor");
        rclcpp::shutdown();
        return 1;
    }

    if (config.enabled_adapter_types.empty()) {
        RCLCPP_WARN(node->get_logger(),
                    "enabled_adapter_types is empty; /start will reject all "
                    "adapter_type values");
    }

    HttpServerRunner runner(node->get_logger());
    if (!runner.Start(config.listen_address, runtime_manager)) {
        rclcpp::shutdown();
        return 1;
    }

    rclcpp::spin(node);

    runner.Stop();
    static_cast<void>(runtime_manager->Stop());
    supervisor->Shutdown();
    rclcpp::shutdown();
    return 0;
}
