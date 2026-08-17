#ifndef REMOTE_CONTROLLER_MQTT_SUBSCRIBER_HPP
#define REMOTE_CONTROLLER_MQTT_SUBSCRIBER_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <mqtt/async_client.h>
#include <nlohmann/json.hpp>

#include "rclcpp/rclcpp.hpp"
#include "remote_controller/config.hpp"

namespace remote_controller
{

class MqttSubscriberManager : public virtual mqtt::callback
{
public:
  using MessageHandler = std::function<nlohmann::json(const std::string &)>;

  MqttSubscriberManager(
    std::shared_ptr<ConfigManager> config_manager,
    rclcpp::Logger logger);

  ~MqttSubscriberManager() override;

  bool start();
  void stop();
  bool isRunning() const {return running_;}

  void setMessageHandler(MessageHandler handler) {message_handler_ = handler;}

  std::string getDownlinkTopic() const;
  std::string getUplinkTopic() const;

private:
  struct QueuedMessage
  {
    std::string topic;
    std::string payload;
  };

  std::shared_ptr<ConfigManager> config_manager_;
  rclcpp::Logger logger_;
  std::shared_ptr<mqtt::async_client> client_;
  std::atomic<bool> running_{false};
  MessageHandler message_handler_;
  std::mutex client_mutex_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<QueuedMessage> message_queue_;
  std::thread worker_thread_;
  bool stop_worker_{false};

  void connected(const std::string & cause) override;
  void connection_lost(const std::string & cause) override;
  void message_arrived(mqtt::const_message_ptr msg) override;

  void startWorkerThread();
  void stopWorkerThread();
  void enqueueMessage(std::string topic, std::string payload);
  void processQueuedMessages();
  void handleQueuedMessage(const QueuedMessage & message);
  void publishResponse(const nlohmann::json & response);
  void subscribeToDownlink();
};

} // namespace remote_controller

#endif // REMOTE_CONTROLLER_MQTT_SUBSCRIBER_HPP
