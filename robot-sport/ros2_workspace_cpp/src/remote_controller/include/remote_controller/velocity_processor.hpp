#ifndef REMOTE_CONTROLLER_VELOCITY_PROCESSOR_HPP
#define REMOTE_CONTROLLER_VELOCITY_PROCESSOR_HPP

#include <memory>
#include <nlohmann/json.hpp>
#include "rclcpp/rclcpp.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "remote_controller/validator.hpp"
#include "remote_controller/response.hpp"
#include "remote_controller/config.hpp"

namespace remote_controller
{

struct ProcessingResult
{
  bool success;
  nlohmann::json response;

  ProcessingResult(bool s, const nlohmann::json & r)
  : success(s), response(r) {}
};

class VelocityProcessor
{
public:
  VelocityProcessor(
    rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr publisher,
    std::shared_ptr<MessageValidator> validator,
    std::shared_ptr<ConfigManager> config_manager,
    rclcpp::Logger logger
  );

  ProcessingResult processVelocityCommand(const std::string & json_payload);

private:
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr publisher_;
  std::shared_ptr<MessageValidator> validator_;
  std::shared_ptr<ConfigManager> config_manager_;
  rclcpp::Logger logger_;

  ProcessingResult handleJsonParseError(const nlohmann::json::parse_error & e);
  ProcessingResult handleValidationError(const ValidationResult & validation_result);
  ProcessingResult handlePublishError(const std::exception & e);
  ProcessingResult buildSuccessResponse(
    const nlohmann::json & velocity_data,
    double processing_time_ms);

  std::unique_ptr<geometry_msgs::msg::Twist> createTwistMessage(
    const nlohmann::json & velocity_data);

  // Helper method to extract velocity values with defaults
  double getVelocityValue(
    const nlohmann::json & json, const std::string & field,
    double default_value = 0.0);
};

} // namespace remote_controller

#endif // REMOTE_CONTROLLER_VELOCITY_PROCESSOR_HPP
