#include <cstdlib>
#include <memory>
#include <nlohmann/json.hpp>

#include "rclcpp/rclcpp.hpp"
#include "geometry_msgs/msg/twist.hpp"
#include "remote_controller/config.hpp"
#include "remote_controller/mqtt_subscriber.hpp"
#include "remote_controller/response.hpp"
#include "remote_controller/validator.hpp"
#include "remote_controller/velocity_processor.hpp"
#include "remote_controller/websocket_server.hpp"

class RemoteController : public rclcpp::Node
{
public:
  RemoteController()
  : Node("remote_controller")
  {
    initializeComponents();
    setupConfiguration();
    setupRosPublisher();
    setupVelocityProcessor();
    setupWebSocketServer();
    setupMqttSubscriber();

    RCLCPP_INFO(this->get_logger(), "Node ready. Listening for WebSocket and MQTT commands...");
  }

  ~RemoteController()
  {
    if (mqtt_subscriber_) {
      mqtt_subscriber_->stop();
    }
    if (websocket_server_) {
      websocket_server_->stop();
    }
  }

private:
  // Initialization methods
  void initializeComponents();
  void setupConfiguration();
  void setupRosPublisher();
  void setupVelocityProcessor();
  void setupWebSocketServer();
  void setupMqttSubscriber();

  // Message processing callback
  nlohmann::json handleVelocityMessage(const std::string & payload);

  // Core components
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr publisher_;
  std::shared_ptr<remote_controller::ConfigManager> config_manager_;
  std::shared_ptr<remote_controller::MessageValidator> validator_;
  std::unique_ptr<remote_controller::VelocityProcessor> velocity_processor_;
  std::unique_ptr<remote_controller::WebSocketServerManager> websocket_server_;
  std::unique_ptr<remote_controller::MqttSubscriberManager> mqtt_subscriber_;
};

// Implementation of private methods
void RemoteController::initializeComponents()
{
  config_manager_ = std::make_shared<remote_controller::ConfigManager>();

  // Configure validator with reasonable velocity limits
  remote_controller::VelocityLimits limits;
  limits.linear_x_min = -5.0;    // m/s
  limits.linear_x_max = 5.0;     // m/s
  limits.angular_z_min = -3.14;   // rad/s
  limits.angular_z_max = 3.14;    // rad/s

  validator_ = std::make_shared<remote_controller::MessageValidator>(limits);
}

void RemoteController::setupConfiguration()
{
  if (!config_manager_->loadConfig()) {
    RCLCPP_WARN(this->get_logger(), "Failed to load configuration, using defaults");
  }

  const auto & config = config_manager_->getConfig();

  // Apply logging level
  std::string level = config.logging.level;
  for (auto & ch : level) {
    ch = static_cast<char>(::toupper(ch));
  }
  if (level == "DEBUG") {
    this->get_logger().set_level(rclcpp::Logger::Level::Debug);
  } else if (level == "WARN") {
    this->get_logger().set_level(rclcpp::Logger::Level::Warn);
  } else if (level == "ERROR") {
    this->get_logger().set_level(rclcpp::Logger::Level::Error);
  } else if (level == "FATAL") {
    this->get_logger().set_level(rclcpp::Logger::Level::Fatal);
  } else {
    this->get_logger().set_level(rclcpp::Logger::Level::Info);
  }

  // Log configuration
  RCLCPP_INFO(this->get_logger(), "[Config] HUB_ID: %s", config.ros.hub_id.c_str());
  RCLCPP_INFO(this->get_logger(), "[Config] WebSocket Port: %d", config.websocket.port);
  RCLCPP_INFO(this->get_logger(), "[Config] WebSocket Host: %s", config.websocket.host.c_str());
  RCLCPP_INFO(
    this->get_logger(), "[Config] Twist Topic Queue Size: %d", config.ros.twist_topic_queue_size);
  RCLCPP_INFO(
    this->get_logger(), "[Config] MQTT Enabled: %s", config.mqtt.enabled ? "true" : "false");
  if (config.mqtt.enabled) {
    RCLCPP_INFO(this->get_logger(), "[Config] MQTT Broker: %s", config.mqtt.broker.c_str());
  }
}

void RemoteController::setupRosPublisher()
{
  const auto & config = config_manager_->getConfig();

  // Build dynamic topic name
  std::string twist_topic = "/" + config.ros.hub_id + "/cmd_vel";

  // Create Twist topic publisher
  publisher_ = create_publisher<geometry_msgs::msg::Twist>(
    twist_topic,
    config.ros.twist_topic_queue_size
  );

  RCLCPP_INFO(
    this->get_logger(),
    "[ROS] cmd_vel publisher ready. topic=%s queue_size=%d",
    twist_topic.c_str(),
    config.ros.twist_topic_queue_size);
}

void RemoteController::setupVelocityProcessor()
{
  velocity_processor_ = std::make_unique<remote_controller::VelocityProcessor>(
    publisher_,
    validator_,
    config_manager_,
    this->get_logger()
  );
}

void RemoteController::setupWebSocketServer()
{
  websocket_server_ = std::make_unique<remote_controller::WebSocketServerManager>(
    config_manager_,
    this->get_logger()
  );

  // Set message handler
  websocket_server_->setMessageHandler(
    [this](const std::string & payload) -> nlohmann::json {
      return handleVelocityMessage(payload);
    }
  );

  // Start the server
  if (!websocket_server_->start()) {
    throw std::runtime_error("Failed to start WebSocket server");
  }
}

void RemoteController::setupMqttSubscriber()
{
  const auto & config = config_manager_->getConfig();
  if (!config.mqtt.enabled) {
    RCLCPP_INFO(this->get_logger(), "MQTT subscriber disabled in config");
    return;
  }

  mqtt_subscriber_ = std::make_unique<remote_controller::MqttSubscriberManager>(
    config_manager_,
    this->get_logger());

  mqtt_subscriber_->setMessageHandler(
    [this](const std::string & payload) -> nlohmann::json {
      return handleVelocityMessage(payload);
    });

  if (!mqtt_subscriber_->start()) {
    RCLCPP_ERROR(
      this->get_logger(),
      "Failed to start MQTT subscriber. Node will continue with WebSocket only.");
  }
}

nlohmann::json RemoteController::handleVelocityMessage(const std::string & payload)
{
  auto result = velocity_processor_->processVelocityCommand(payload);
  return result.response;
}

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<RemoteController>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
