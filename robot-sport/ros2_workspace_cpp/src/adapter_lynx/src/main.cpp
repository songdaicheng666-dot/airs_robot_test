#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "adapter_lynx/lynx_adapter_node.hpp"

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<adapter_lynx::LynxAdapterNode>();
    node->Init();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
