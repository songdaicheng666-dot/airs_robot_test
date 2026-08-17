#include "remote_controller/config.hpp"
#include "remote_controller/device_info.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>

namespace remote_controller
{

ConfigManager::ConfigManager()
{
  loadDefaults();
}

bool ConfigManager::loadConfig(const std::string & config_file_path)
{
  // First load defaults
  loadDefaults();

  // Determine which config file to use
  std::string config_path = config_file_path;

  // If no explicit path provided, check environment variable
  if (config_path.empty()) {
    const char * env_config_path = std::getenv("REMOTE_CONTROLLER_CONFIG");
    if (env_config_path) {
      config_path = std::string(env_config_path);
      std::cout << "Using config file from REMOTE_CONTROLLER_CONFIG: " << config_path << std::endl;
    }
  }

  // Try to load from specified or environment config path
  if (!config_path.empty()) {
    if (!overrideFromFile(config_path)) {
      std::cerr << "Warning: Could not load config file: " << config_path << std::endl;
    }
  } else {
    // Fallback to default location
    std::string default_path = "config/remote_controller_config.json";
    if (std::filesystem::exists(default_path)) {
      overrideFromFile(default_path);
      std::cout << "Using default config file: " << default_path << std::endl;
    } else {
      std::cout << "No config file found, using defaults only" << std::endl;
    }
  }

  // Override with environment variables (excluding HUB_ID, which is read from device info only)
  overrideFromEnvironment();

  // Read hub_id from device info file (SN field) - the only source for hub_id
  std::string sn_from_device = DeviceInfoReader::readSerialNumber();
  if (!sn_from_device.empty()) {
    config_.ros.hub_id = sn_from_device;
    std::cout << "Using SN from device_info.json as hub_id: " << sn_from_device << std::endl;
  } else {
    std::cerr << "Warning: Could not read SN from device_info.json, using default hub_id" <<
      std::endl;
  }

  return true;
}

void ConfigManager::loadDefaults()
{
  // WebSocket defaults
  config_.websocket.port = 9099;
  config_.websocket.host = "0.0.0.0";
  config_.websocket.max_connections = 10;

  // ROS defaults
  config_.ros.twist_topic_queue_size = 10;
  config_.ros.hub_id = "DEFAULT_HUB_ID";

  // Logging defaults
  config_.logging.level = "INFO";
  config_.logging.enable_websocket_logs = true;

  // MQTT defaults
  config_.mqtt.enabled = false;
  config_.mqtt.broker = "tcp://localhost:1883";
  config_.mqtt.region = "";
  config_.mqtt.tenant_id = "";
  config_.mqtt.username = "";
  config_.mqtt.password = "";
  config_.mqtt.qos = 1;
  config_.mqtt.keep_alive_interval = 20;
  config_.mqtt.clean_session = true;
}

void ConfigManager::overrideFromEnvironment()
{
  // ROS environment variables
  // Note: HUB_ID is intentionally NOT read from environment - it must come from device_info.json SN field only

  int queue_size = getEnvVarInt("TWIST_TOPIC_QUEUE_SIZE", config_.ros.twist_topic_queue_size);
  if (queue_size > 0) {
    config_.ros.twist_topic_queue_size = queue_size;
  }

  // MQTT environment variables
  std::string mqtt_broker = getEnvVar("MQTT_BROKER");
  if (!mqtt_broker.empty()) {
    config_.mqtt.broker = mqtt_broker;
  }

  std::string mqtt_region = getEnvVar("MQTT_REGION");
  if (!mqtt_region.empty()) {
    config_.mqtt.region = mqtt_region;
  }

  std::string mqtt_tenant_id = getEnvVar("MQTT_TENANT_ID");
  if (!mqtt_tenant_id.empty()) {
    config_.mqtt.tenant_id = mqtt_tenant_id;
  }

  std::string mqtt_username = getEnvVar("MQTT_USERNAME");
  if (!mqtt_username.empty()) {
    config_.mqtt.username = mqtt_username;
  }

  std::string mqtt_password = getEnvVar("MQTT_PASSWORD");
  if (!mqtt_password.empty()) {
    config_.mqtt.password = mqtt_password;
  }
}

bool ConfigManager::overrideFromFile(const std::string & file_path)
{
  try {
    std::ifstream file(file_path);
    if (!file.is_open()) {
      return false;
    }

    nlohmann::json j;
    file >> j;

    // WebSocket config
    if (j.contains("websocket")) {
      auto & ws = j["websocket"];
      if (ws.contains("port")) {config_.websocket.port = ws["port"];}
      if (ws.contains("host")) {config_.websocket.host = ws["host"];}
      if (ws.contains("max_connections")) {
        config_.websocket.max_connections = ws["max_connections"];
      }
    }

    // ROS config
    if (j.contains("ros")) {
      auto & ros = j["ros"];
      if (ros.contains("twist_topic_queue_size")) {
        config_.ros.twist_topic_queue_size = ros["twist_topic_queue_size"];
      }
      // Note: hub_id is intentionally NOT read from config file - it must come from device_info.json SN field only
    }

    // Logging config
    if (j.contains("logging")) {
      auto & log = j["logging"];
      if (log.contains("level")) {config_.logging.level = log["level"];}
      if (log.contains("enable_websocket_logs")) {
        config_.logging.enable_websocket_logs = log["enable_websocket_logs"];
      }
    }

    // MQTT config
    if (j.contains("mqtt")) {
      auto & mq = j["mqtt"];
      if (mq.contains("enabled")) {config_.mqtt.enabled = mq["enabled"];}
      if (mq.contains("broker")) {config_.mqtt.broker = mq["broker"];}
      if (mq.contains("region")) {config_.mqtt.region = mq["region"];}
      if (mq.contains("tenant_id")) {config_.mqtt.tenant_id = mq["tenant_id"];}
      if (mq.contains("username")) {config_.mqtt.username = mq["username"];}
      if (mq.contains("password")) {config_.mqtt.password = mq["password"];}
      if (mq.contains("qos")) {config_.mqtt.qos = mq["qos"];}
      if (mq.contains("keep_alive_interval")) {
        config_.mqtt.keep_alive_interval = mq["keep_alive_interval"];
      }
      if (mq.contains("clean_session")) {
        config_.mqtt.clean_session = mq["clean_session"];
      }
    }

    return true;
  } catch (const std::exception & e) {
    std::cerr << "Error loading config file: " << e.what() << std::endl;
    return false;
  }
}

std::string ConfigManager::getEnvVar(const std::string & name, const std::string & default_value)
{
  const char * value = std::getenv(name.c_str());
  return value ? std::string(value) : default_value;
}

int ConfigManager::getEnvVarInt(const std::string & name, int default_value)
{
  const char * value = std::getenv(name.c_str());
  if (!value) {return default_value;}

  try {
    return std::stoi(value);
  } catch (const std::exception &) {
    return default_value;
  }
}

bool ConfigManager::getEnvVarBool(const std::string & name, bool default_value)
{
  const char * value = std::getenv(name.c_str());
  if (!value) {return default_value;}

  std::string str_value(value);
  std::transform(str_value.begin(), str_value.end(), str_value.begin(), ::tolower);

  return str_value == "true" || str_value == "1" || str_value == "yes";
}

} // namespace remote_controller
