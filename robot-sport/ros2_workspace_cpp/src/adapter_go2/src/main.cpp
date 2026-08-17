#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "adapter_go2/go2_adapter_node.hpp"

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<adapter_go2::Go2AdapterNode>();
    node->Init();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
