#pragma once

#include <memory>
#include <rclcpp/logger.hpp>
#include <string>

namespace robot_switch_server {

// Forward declarations to reduce header coupling
class AdapterRuntimeManager;

class HttpServerRunner {
public:
    explicit HttpServerRunner(rclcpp::Logger logger);
    ~HttpServerRunner();

    bool Start(const std::string& listen_address,
               const std::shared_ptr<AdapterRuntimeManager>& manager);
    void Stop();

private:
    struct Impl;
    rclcpp::Logger logger_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace robot_switch_server
