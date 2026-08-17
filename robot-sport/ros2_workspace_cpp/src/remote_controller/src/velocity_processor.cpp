#include "remote_controller/velocity_processor.hpp"
#include <chrono>

namespace remote_controller
{

VelocityProcessor::VelocityProcessor(
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr publisher,
  std::shared_ptr<MessageValidator> validator,
  std::shared_ptr<ConfigManager> config_manager,
  rclcpp::Logger logger
)
: publisher_(publisher),
  validator_(validator),
  config_manager_(config_manager),
  logger_(logger) {}

ProcessingResult VelocityProcessor::processVelocityCommand(const std::string & json_payload)
{
  auto start_time = std::chrono::high_resolution_clock::now();

  try {
    // Parse JSON request
    nlohmann::json payload;
    try {
      payload = nlohmann::json::parse(json_payload);
    } catch (const nlohmann::json::parse_error & e) {
      return handleJsonParseError(e);
    }

    // Validate input
    auto validation_result = validator_->validateVelocityCommand(payload);
    if (!validation_result.is_valid) {
      return handleValidationError(validation_result);
    }

    // Extract validated values - support all 6 DOF with backward compatibility
    nlohmann::json velocity_data;
    velocity_data["linear_x"] = getVelocityValue(payload, "linear_x");
    velocity_data["linear_y"] = getVelocityValue(payload, "linear_y");
    velocity_data["linear_z"] = getVelocityValue(payload, "linear_z");
    velocity_data["angular_x"] = getVelocityValue(payload, "angular_x");
    velocity_data["angular_y"] = getVelocityValue(payload, "angular_y");
    velocity_data["angular_z"] = getVelocityValue(payload, "angular_z");

    // Create and publish Twist message
    auto twist_msg = createTwistMessage(velocity_data);
    const std::string topic_name = publisher_ ? publisher_->get_topic_name() : "<null>";

    try {
      publisher_->publish(std::move(twist_msg));
    } catch (const std::exception & e) {
      return handlePublishError(e);
    }

    RCLCPP_DEBUG(
      logger_,
      "[VelocityProcessor] Forwarded velocity to %s: linear=[%.2f, %.2f, %.2f], angular=[%.2f, %.2f, %.2f]",
      topic_name.c_str(),
      velocity_data["linear_x"].get<double>(),
      velocity_data["linear_y"].get<double>(), velocity_data["linear_z"].get<double>(),
      velocity_data["angular_x"].get<double>(),
      velocity_data["angular_y"].get<double>(), velocity_data["angular_z"].get<double>());

    // Calculate processing time
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    double processing_time_ms = duration.count() / 1000.0;

    return buildSuccessResponse(velocity_data, processing_time_ms);

  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger_, "Unexpected error processing velocity command: %s", e.what());

    ResponseBuilder response_builder;
    auto response = response_builder.buildError(
      ResponseStatus::INTERNAL_ERROR,
      ErrorCode::INTERNAL_PROCESSING_ERROR,
      "Internal server error occurred"
    );
    return ProcessingResult(false, response);
  }
}

ProcessingResult VelocityProcessor::handleJsonParseError(const nlohmann::json::parse_error & e)
{
  RCLCPP_WARN(logger_, "[VelocityProcessor] JSON parse failed: %s", e.what());
  ResponseBuilder response_builder;
  auto response = response_builder.buildError(
    ResponseStatus::BAD_REQUEST,
    ErrorCode::INVALID_JSON_FORMAT,
    "Invalid JSON format: " + std::string(e.what()),
    ""
  );
  return ProcessingResult(false, response);
}

ProcessingResult VelocityProcessor::handleValidationError(
  const ValidationResult & validation_result)
{
  ResponseBuilder response_builder;
  // Send first validation error (could be enhanced to send all errors)
  const auto & first_error = validation_result.errors[0];
  RCLCPP_WARN(
    logger_,
    "[VelocityProcessor] Validation failed. code=%d field=%s message=%s",
    static_cast<int>(first_error.code),
    first_error.field.c_str(),
    first_error.message.c_str());
  auto response = response_builder.buildError(
    ResponseStatus::VALIDATION_ERROR,
    first_error
  );
  return ProcessingResult(false, response);
}

ProcessingResult VelocityProcessor::handlePublishError(const std::exception & e)
{
  const std::string topic_name = publisher_ ? publisher_->get_topic_name() : "<null>";
  RCLCPP_ERROR(
    logger_,
    "[VelocityProcessor] ROS publish failed for topic %s: %s",
    topic_name.c_str(),
    e.what());
  ResponseBuilder response_builder;
  auto response = response_builder.buildError(
    ResponseStatus::INTERNAL_ERROR,
    ErrorCode::ROS_PUBLISH_FAILED,
    "Failed to publish ROS message: " + std::string(e.what())
  );
  return ProcessingResult(false, response);
}

ProcessingResult VelocityProcessor::buildSuccessResponse(
  const nlohmann::json & velocity_data,
  double processing_time_ms)
{
  const auto & config = config_manager_->getConfig();
  std::string topic = "/" + config.ros.hub_id + "/cmd_vel";

  VelocityData vel_data{
    velocity_data["linear_x"].get<double>(),
    velocity_data["linear_y"].get<double>(),
    velocity_data["linear_z"].get<double>(),
    velocity_data["angular_x"].get<double>(),
    velocity_data["angular_y"].get<double>(),
    velocity_data["angular_z"].get<double>(),
    topic, config.ros.hub_id
  };

  ResponseBuilder response_builder;
  auto response = response_builder
    .setProcessingTime(processing_time_ms)
    .buildSuccess(vel_data);

  return ProcessingResult(true, response);
}

std::unique_ptr<geometry_msgs::msg::Twist> VelocityProcessor::createTwistMessage(
  const nlohmann::json & velocity_data)
{
  auto twist_msg = std::make_unique<geometry_msgs::msg::Twist>();
  twist_msg->linear.x = velocity_data["linear_x"].get<double>();
  twist_msg->linear.y = velocity_data["linear_y"].get<double>();
  twist_msg->linear.z = velocity_data["linear_z"].get<double>();
  twist_msg->angular.x = velocity_data["angular_x"].get<double>();
  twist_msg->angular.y = velocity_data["angular_y"].get<double>();
  twist_msg->angular.z = velocity_data["angular_z"].get<double>();
  return twist_msg;
}

double VelocityProcessor::getVelocityValue(
  const nlohmann::json & json, const std::string & field,
  double default_value)
{
  if (json.contains(field) && json[field].is_number()) {
    return json[field].get<double>();
  }
  return default_value;
}

} // namespace remote_controller
