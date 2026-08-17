#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "adapter_scout/scout_adapter_node.hpp"

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<adapter_scout::ScoutAdapterNode>();
    node->Init();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
