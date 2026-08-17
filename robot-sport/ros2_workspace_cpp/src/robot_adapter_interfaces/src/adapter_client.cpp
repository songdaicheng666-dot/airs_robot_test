#include "robot_adapter_interfaces/adapter_client.hpp"

#include <exception>
#include <utility>

namespace robot_adapter_interfaces {

namespace {
// Monotonic counter for unique node names
std::atomic<uint64_t> g_rpc_node_counter{0};
}  // namespace

AdapterClient::AdapterClient(rclcpp::Node::SharedPtr /*parent_node*/,
                             std::string adapter_name,
                             std::string service_prefix,
                             std::chrono::milliseconds service_wait,
                             std::chrono::milliseconds call_timeout)
    : adapter_name_(std::move(adapter_name))
    , service_prefix_(std::move(service_prefix))
    , service_wait_(service_wait)
    , call_timeout_(call_timeout) {
    // Create a dedicated node for RPC calls — avoids executor conflict
    const auto id = g_rpc_node_counter.fetch_add(1);
    const std::string node_name =
        "rpc_client_" + adapter_name_ + "_" + std::to_string(id);
    rpc_node_ = std::make_shared<rclcpp::Node>(node_name);

    connect_client_ = rpc_node_->create_client<std_srvs::srv::Trigger>(
        service_prefix_ + "/connect");
    disconnect_client_ = rpc_node_->create_client<std_srvs::srv::Trigger>(
        service_prefix_ + "/disconnect");
    safe_stop_client_ = rpc_node_->create_client<std_srvs::srv::Trigger>(
        service_prefix_ + "/safe_stop");
    health_client_ = rpc_node_->create_client<std_srvs::srv::Trigger>(
        service_prefix_ + "/health");
    system_info_client_ = rpc_node_->create_client<std_srvs::srv::Trigger>(
        service_prefix_ + "/system_info");

    executor_.add_node(rpc_node_);
    spin_thread_ = std::thread(&AdapterClient::SpinThread, this);
}

AdapterClient::~AdapterClient() {
    shutdown_.store(true);
    executor_.cancel();
    if (spin_thread_.joinable()) {
        spin_thread_.join();
    }
    executor_.remove_node(rpc_node_);
}

void AdapterClient::SpinThread() {
    while (!shutdown_.load()) {
        executor_.spin_once(std::chrono::milliseconds(50));
    }
}

AdapterCallResult AdapterClient::Connect() {
    return CallTrigger("connect", connect_client_);
}

AdapterCallResult AdapterClient::Disconnect() {
    return CallTrigger("disconnect", disconnect_client_);
}

AdapterCallResult AdapterClient::SafeStop() {
    return CallTrigger("safe_stop", safe_stop_client_);
}

AdapterCallResult AdapterClient::Health() {
    return CallTrigger("health", health_client_);
}

AdapterCallResult AdapterClient::SystemInfo() {
    return CallTrigger("system_info", system_info_client_);
}

AdapterCallResult AdapterClient::CallTriggerByName(
    const std::string& service_suffix) {
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr client;
    {
        std::lock_guard<std::mutex> lock(dynamic_clients_mutex_);
        auto it = dynamic_clients_.find(service_suffix);
        if (it == dynamic_clients_.end()) {
            client = rpc_node_->create_client<std_srvs::srv::Trigger>(
                service_prefix_ + "/" + service_suffix);
            dynamic_clients_.emplace(service_suffix, client);
        } else {
            client = it->second;
        }
    }
    return CallTrigger(service_suffix, client);
}

AdapterCallResult AdapterClient::CallTrigger(
    const std::string& action,
    const rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr& client) {
    std::lock_guard<std::mutex> lock(call_mutex_);

    if (!client->wait_for_service(service_wait_)) {
        return AdapterCallResult{false, false,
                                 "service unreachable: " + service_prefix_ +
                                     "/" + action,
                                 AdapterCallFailure::kUnreachable};
    }

    auto request = std::make_shared<std_srvs::srv::Trigger::Request>();
    auto future = client->async_send_request(request);

    // Wait on the shared_future — the executor spin thread processes callbacks
    const auto status = future.wait_for(call_timeout_);

    if (status != std::future_status::ready) {
        return AdapterCallResult{false, true,
                                 "service call timeout/failure: " +
                                     service_prefix_ + "/" + action,
                                 AdapterCallFailure::kCallFailed};
    }

    try {
        const auto response = future.get();
        return AdapterCallResult{
            response->success,
            true,
            response->message,
            response->success ? AdapterCallFailure::kNone
                              : AdapterCallFailure::kRejected};
    } catch (const std::exception& ex) {
        return AdapterCallResult{
            false,
            true,
            "service call failure: " + service_prefix_ + "/" + action +
                " (" + ex.what() + ")",
            AdapterCallFailure::kCallFailed};
    } catch (...) {
        return AdapterCallResult{
            false,
            true,
            "service call failure: " + service_prefix_ + "/" + action,
            AdapterCallFailure::kCallFailed};
    }
}

}  // namespace robot_adapter_interfaces
