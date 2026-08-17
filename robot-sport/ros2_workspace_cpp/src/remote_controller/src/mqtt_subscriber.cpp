#include "remote_controller/mqtt_subscriber.hpp"

#include <chrono>
#include <utility>

namespace remote_controller
{
namespace
{

constexpr auto kConnectTimeout = std::chrono::seconds(5);
constexpr auto kShutdownTimeout = std::chrono::seconds(2);

std::string extractResponseMessage(const nlohmann::json & response)
{
  if (response.contains("msg") && response["msg"].is_string()) {
    return response["msg"].get<std::string>();
  }
  return "N/A";
}

} // namespace

MqttSubscriberManager::MqttSubscriberManager(
  std::shared_ptr<ConfigManager> config_manager,
  rclcpp::Logger logger)
: config_manager_(config_manager),
  logger_(logger)
{
}

MqttSubscriberManager::~MqttSubscriberManager()
{
  stop();
}

std::string MqttSubscriberManager::getDownlinkTopic() const
{
  const auto & config = config_manager_->getConfig();
  return "sys/" + config.mqtt.region + "/" + config.mqtt.tenant_id + "/" +
         config.ros.hub_id + "/remote_control/downlink";
}

std::string MqttSubscriberManager::getUplinkTopic() const
{
  const auto & config = config_manager_->getConfig();
  return "sys/" + config.mqtt.region + "/" + config.mqtt.tenant_id + "/" +
         config.ros.hub_id + "/remote_control/uplink";
}

bool MqttSubscriberManager::start()
{
  if (running_.load()) {
    RCLCPP_WARN(logger_, "MQTT subscriber is already running");
    return false;
  }

  const auto & mqtt_config = config_manager_->getMqttConfig();
  const auto & ros_config = config_manager_->getROSConfig();
  const std::string client_id = "remote_controller_" + ros_config.hub_id;

  startWorkerThread();

  try {
    auto client = std::make_shared<mqtt::async_client>(mqtt_config.broker, client_id);
    client->set_callback(*this);
    {
      std::lock_guard<std::mutex> lock(client_mutex_);
      client_ = client;
    }

    auto conn_opts_builder = mqtt::connect_options_builder()
      .keep_alive_interval(std::chrono::seconds(mqtt_config.keep_alive_interval))
      .clean_session(mqtt_config.clean_session)
      .automatic_reconnect(true);

    if (!mqtt_config.username.empty()) {
      conn_opts_builder.user_name(mqtt_config.username);
      conn_opts_builder.password(mqtt_config.password);
    }

    auto conn_opts = conn_opts_builder.finalize();

    RCLCPP_INFO(logger_, "Connecting to MQTT broker: %s", mqtt_config.broker.c_str());
    auto connect_token = client->connect(conn_opts);
    if (!connect_token->wait_for(kConnectTimeout)) {
      RCLCPP_ERROR(logger_, "Timed out connecting to MQTT broker: %s", mqtt_config.broker.c_str());
      stop();
      return false;
    }

    auto subscribe_token = client->subscribe(getDownlinkTopic(), mqtt_config.qos);
    if (!subscribe_token->wait_for(kConnectTimeout)) {
      RCLCPP_ERROR(logger_, "Timed out subscribing to MQTT topic: %s", getDownlinkTopic().c_str());
      stop();
      return false;
    }

    running_.store(true);
    RCLCPP_INFO(logger_, "MQTT subscriber started. Downlink: %s", getDownlinkTopic().c_str());
    RCLCPP_INFO(logger_, "MQTT subscriber uplink topic: %s", getUplinkTopic().c_str());
    return true;
  } catch (const mqtt::exception & e) {
    RCLCPP_ERROR(logger_, "MQTT startup error: %s", e.what());
  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger_, "Unexpected MQTT startup error: %s", e.what());
  }

  stop();
  return false;
}

void MqttSubscriberManager::stop()
{
  std::shared_ptr<mqtt::async_client> client;
  {
    std::lock_guard<std::mutex> lock(client_mutex_);
    client = client_;
  }

  if (!client && !running_.load()) {
    stopWorkerThread();
    return;
  }

  running_.store(false);

  try {
    if (client && client->is_connected()) {
      auto unsubscribe_token = client->unsubscribe(getDownlinkTopic());
      if (!unsubscribe_token->wait_for(kShutdownTimeout)) {
        RCLCPP_WARN(logger_, "Timed out unsubscribing from MQTT downlink topic");
      }

      auto disconnect_token = client->disconnect();
      if (!disconnect_token->wait_for(kShutdownTimeout)) {
        RCLCPP_WARN(logger_, "Timed out disconnecting from MQTT broker");
      }
    }
  } catch (const mqtt::exception & e) {
    RCLCPP_WARN(logger_, "Error during MQTT shutdown: %s", e.what());
  }

  stopWorkerThread();
  {
    std::lock_guard<std::mutex> lock(client_mutex_);
    client_.reset();
  }
  RCLCPP_INFO(logger_, "MQTT subscriber stopped");
}

void MqttSubscriberManager::connected(const std::string & cause)
{
  RCLCPP_INFO(logger_, "MQTT connected. Cause: %s", cause.c_str());
  if (running_.load()) {
    subscribeToDownlink();
  }
}

void MqttSubscriberManager::connection_lost(const std::string & cause)
{
  RCLCPP_WARN(
    logger_,
    "MQTT connection lost. Cause: %s",
    cause.empty() ? "unknown" : cause.c_str());
}

void MqttSubscriberManager::message_arrived(mqtt::const_message_ptr msg)
{
  if (!msg) {
    RCLCPP_WARN(logger_, "Received null MQTT message");
    return;
  }

  if (!message_handler_) {
    RCLCPP_ERROR(logger_, "No message handler set for MQTT subscriber");
    return;
  }

  try {
    const std::string payload = msg->to_string();
    enqueueMessage(msg->get_topic(), payload);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(logger_, "Error queueing MQTT message: %s", e.what());
  }
}

void MqttSubscriberManager::startWorkerThread()
{
  stopWorkerThread();

  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stop_worker_ = false;
    message_queue_.clear();
  }

  worker_thread_ = std::thread(&MqttSubscriberManager::processQueuedMessages, this);
}

void MqttSubscriberManager::stopWorkerThread()
{
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stop_worker_ = true;
    message_queue_.clear();
  }
  queue_cv_.notify_all();

  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

void MqttSubscriberManager::enqueueMessage(std::string topic, std::string payload)
{
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (stop_worker_) {
      RCLCPP_WARN(logger_, "Dropping MQTT message because worker is stopping");
      return;
    }

    message_queue_.push_back(QueuedMessage{std::move(topic), std::move(payload)});
  }

  queue_cv_.notify_one();
}

void MqttSubscriberManager::processQueuedMessages()
{
  while (true) {
    QueuedMessage message;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this]() {
        return stop_worker_ || !message_queue_.empty();
      });

      if (stop_worker_) {
        return;
      }

      message = std::move(message_queue_.front());
      message_queue_.pop_front();
    }

    handleQueuedMessage(message);
  }
}

void MqttSubscriberManager::handleQueuedMessage(const QueuedMessage & message)
{
  try {
    nlohmann::json response = message_handler_(message.payload);
    const std::string response_message = extractResponseMessage(response);

    publishResponse(response);
  } catch (const std::exception & e) {
    RCLCPP_ERROR(
      logger_,
      "Error handling queued MQTT message for topic %s: %s",
      message.topic.c_str(),
      e.what());
  }
}

void MqttSubscriberManager::publishResponse(const nlohmann::json & response)
{
  std::shared_ptr<mqtt::async_client> client;
  {
    std::lock_guard<std::mutex> lock(client_mutex_);
    client = client_;
  }

  if (!client || !client->is_connected()) {
    RCLCPP_WARN(logger_, "Skipping MQTT response publish because client is disconnected");
    return;
  }

  try {
    const auto & mqtt_config = config_manager_->getMqttConfig();
    const std::string response_payload = response.dump();
    auto mqtt_response_message = mqtt::make_message(getUplinkTopic(), response_payload);
    mqtt_response_message->set_qos(mqtt_config.qos);
    client->publish(mqtt_response_message);
  } catch (const mqtt::exception & e) {
    RCLCPP_WARN(logger_, "Failed to publish MQTT response: %s", e.what());
  }
}

void MqttSubscriberManager::subscribeToDownlink()
{
  std::shared_ptr<mqtt::async_client> client;
  {
    std::lock_guard<std::mutex> lock(client_mutex_);
    client = client_;
  }

  if (!client || !client->is_connected()) {
    return;
  }

  try {
    const auto & mqtt_config = config_manager_->getMqttConfig();
    auto subscribe_token = client->subscribe(getDownlinkTopic(), mqtt_config.qos);
    if (!subscribe_token->wait_for(kConnectTimeout)) {
      RCLCPP_ERROR(
        logger_, "Timed out re-subscribing to MQTT topic: %s",
        getDownlinkTopic().c_str());
      return;
    }

    RCLCPP_INFO(logger_, "MQTT re-subscribed to: %s", getDownlinkTopic().c_str());
  } catch (const mqtt::exception & e) {
    RCLCPP_ERROR(logger_, "MQTT re-subscribe failed: %s", e.what());
  }
}

} // namespace remote_controller
