#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <string>
#include <thread>
#include <unordered_map>

namespace robot_adapter_interfaces {

enum class AdapterCallFailure {
    kNone,
    kRejected,
    kUnreachable,
    kCallFailed,
};

struct AdapterCallResult {
    bool ok{false};
    bool reachable{false};
    std::string message;
    AdapterCallFailure failure{AdapterCallFailure::kNone};
};

class AdapterClient {
public:
    AdapterClient(
        rclcpp::Node::SharedPtr parent_node, std::string adapter_name,
        std::string service_prefix,
        std::chrono::milliseconds service_wait = std::chrono::milliseconds(500),
        std::chrono::milliseconds call_timeout =
            std::chrono::milliseconds(1200));

    ~AdapterClient();

    AdapterCallResult Connect();
    AdapterCallResult Disconnect();
    AdapterCallResult SafeStop();
    AdapterCallResult Health();
    AdapterCallResult SystemInfo();

    // Generic Trigger call by service suffix. The full service path is
    // `service_prefix + "/" + service_suffix`. The underlying client is created
    // on first use and cached for subsequent calls with the same suffix.
    // Timeout matches `call_timeout` from construction.
    //
    // Thread-safety: safe to call concurrently from multiple threads.
    // `service_suffix` must be non-empty and should come from the adapter's
    // declared motion set (validated upstream by the caller).
    // Creating a client while the executor spin thread is running is safe:
    // rclcpp::NodeServices::add_client() adds the client under the
    // CallbackGroup's mutex and then triggers the node's notify guard
    // condition, which causes the executor to rebuild its wait set on the
    // next spin iteration — the standard rclcpp dynamic-entity mechanism.
    AdapterCallResult CallTriggerByName(const std::string& service_suffix);

    const std::string& name() const { return adapter_name_; }

private:
    AdapterCallResult CallTrigger(
        const std::string& action,
        const rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr& client);

    void SpinThread();

    std::string adapter_name_;
    std::string service_prefix_;
    std::chrono::milliseconds service_wait_;
    std::chrono::milliseconds call_timeout_;
    std::mutex call_mutex_;

    // Independent node + executor (avoids executor conflict with main node)
    rclcpp::Node::SharedPtr rpc_node_;
    rclcpp::executors::SingleThreadedExecutor executor_;
    std::thread spin_thread_;
    std::atomic<bool> shutdown_{false};

    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr connect_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr disconnect_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr safe_stop_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr health_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr system_info_client_;

    // Cache of clients created on demand by CallTriggerByName.
    std::mutex dynamic_clients_mutex_;
    std::unordered_map<std::string,
                       rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr>
        dynamic_clients_;
};

}  // namespace robot_adapter_interfaces
