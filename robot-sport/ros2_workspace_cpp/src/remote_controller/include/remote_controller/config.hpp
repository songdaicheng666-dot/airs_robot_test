#ifndef REMOTE_CONTROLLER_CONFIG_HPP
#define REMOTE_CONTROLLER_CONFIG_HPP

#include <string>
#include <nlohmann/json.hpp>
#include <memory>

namespace remote_controller
{

struct WebSocketConfig
{
  int port;
  std::string host;
  int max_connections;
};

struct ROSConfig
{
  int twist_topic_queue_size;
  std::string hub_id;
};

struct LoggingConfig
{
  std::string level;
  bool enable_websocket_logs;
};

struct MqttConfig
{
  bool enabled{false};
  std::string broker{"tcp://localhost:1883"};
  std::string region;
  std::string tenant_id;
  std::string username;
  std::string password;
  int qos{1};
  int keep_alive_interval{20};
  bool clean_session{true};
};

struct Config
{
  WebSocketConfig websocket;
  ROSConfig ros;
  LoggingConfig logging;
  MqttConfig mqtt;
};

class ConfigManager
{
public:
  ConfigManager();
  ~ConfigManager() = default;

  // Load configuration from file and environment variables
  bool loadConfig(const std::string & config_file_path = "");

  // Get configuration
  const Config & getConfig() const {return config_;}

  // Get specific config sections
  const WebSocketConfig & getWebSocketConfig() const {return config_.websocket;}
  const ROSConfig & getROSConfig() const {return config_.ros;}
  const LoggingConfig & getLoggingConfig() const {return config_.logging;}
  const MqttConfig & getMqttConfig() const {return config_.mqtt;}

private:
  Config config_;

  // Load default configuration
  void loadDefaults();

  // Override with environment variables
  void overrideFromEnvironment();

  // Override with JSON file
  bool overrideFromFile(const std::string & file_path);

  // Helper methods
  std::string getEnvVar(const std::string & name, const std::string & default_value = "");
  int getEnvVarInt(const std::string & name, int default_value = 0);
  bool getEnvVarBool(const std::string & name, bool default_value = false);
};

} // namespace remote_controller

#endif // REMOTE_CONTROLLER_CONFIG_HPP
