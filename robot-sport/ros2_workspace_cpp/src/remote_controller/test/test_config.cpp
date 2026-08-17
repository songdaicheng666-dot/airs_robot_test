#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "remote_controller/config.hpp"

class ConfigTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    // Store original environment variables
    original_hub_id_ = getEnvVarOrEmpty("HUB_ID");
    original_twist_topic_queue_size_ = getEnvVarOrEmpty("TWIST_TOPIC_QUEUE_SIZE");
    original_remote_controller_config_ = getEnvVarOrEmpty("REMOTE_CONTROLLER_CONFIG");
    original_mqtt_broker_ = getEnvVarOrEmpty("MQTT_BROKER");
    original_mqtt_region_ = getEnvVarOrEmpty("MQTT_REGION");
    original_mqtt_tenant_id_ = getEnvVarOrEmpty("MQTT_TENANT_ID");
    original_mqtt_username_ = getEnvVarOrEmpty("MQTT_USERNAME");
    original_mqtt_password_ = getEnvVarOrEmpty("MQTT_PASSWORD");

    // Clear environment variables for clean testing
    unsetenv("HUB_ID");
    unsetenv("TWIST_TOPIC_QUEUE_SIZE");
    unsetenv("REMOTE_CONTROLLER_CONFIG");
    unsetenv("MQTT_BROKER");
    unsetenv("MQTT_REGION");
    unsetenv("MQTT_TENANT_ID");
    unsetenv("MQTT_USERNAME");
    unsetenv("MQTT_PASSWORD");

    // Create a temporary config file for testing
    test_config_content =
      R"({
            "websocket": {
                "port": 8080,
                "host": "127.0.0.1",
                "max_connections": 5
            },
            "ros": {
                "twist_topic_queue_size": 5,
                "hub_id": "TEST_HUB"
            },
            "logging": {
                "level": "DEBUG",
                "enable_websocket_logs": false
            },
            "mqtt": {
                "enabled": true,
                "broker": "tcp://broker.example.com:1883",
                "region": "us-east-1",
                "tenant_id": "tenant-abc",
                "username": "user1",
                "password": "pass1",
                "qos": 2,
                "keep_alive_interval": 30,
                "clean_session": false
            }
        })";

    test_config_path = "test_config.json";
    writeFile(test_config_path, test_config_content);
  }

  void TearDown() override
  {
    // Clean up test file
    if (std::filesystem::exists(test_config_path)) {
      std::filesystem::remove(test_config_path);
    }

    // Restore original environment variables
    restoreEnvVar("HUB_ID", original_hub_id_);
    restoreEnvVar("TWIST_TOPIC_QUEUE_SIZE", original_twist_topic_queue_size_);
    restoreEnvVar("REMOTE_CONTROLLER_CONFIG", original_remote_controller_config_);
    restoreEnvVar("MQTT_BROKER", original_mqtt_broker_);
    restoreEnvVar("MQTT_REGION", original_mqtt_region_);
    restoreEnvVar("MQTT_TENANT_ID", original_mqtt_tenant_id_);
    restoreEnvVar("MQTT_USERNAME", original_mqtt_username_);
    restoreEnvVar("MQTT_PASSWORD", original_mqtt_password_);
  }

  std::string test_config_content;
  std::string test_config_path;

  static std::string getEnvVarOrEmpty(const std::string & name)
  {
    const char * value = std::getenv(name.c_str());
    return value ? std::string(value) : std::string();
  }

  static void restoreEnvVar(const std::string & name, const std::string & value)
  {
    if (!value.empty()) {
      setenv(name.c_str(), value.c_str(), 1);
    } else {
      unsetenv(name.c_str());
    }
  }

  static void writeFile(const std::string & path, const std::string & content)
  {
    std::ofstream file(path);
    ASSERT_TRUE(file.is_open());
    file << content;
  }

  // Store original environment variables
  std::string original_hub_id_;
  std::string original_twist_topic_queue_size_;
  std::string original_remote_controller_config_;
  std::string original_mqtt_broker_;
  std::string original_mqtt_region_;
  std::string original_mqtt_tenant_id_;
  std::string original_mqtt_username_;
  std::string original_mqtt_password_;
};

TEST_F(ConfigTest, DefaultConfiguration) {
  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig();

  const auto & config = config_manager.getConfig();

  EXPECT_EQ(config.websocket.port, 9099);
  EXPECT_EQ(config.websocket.host, "0.0.0.0");
  EXPECT_EQ(config.websocket.max_connections, 10);
  EXPECT_EQ(config.ros.twist_topic_queue_size, 10);
  EXPECT_EQ(config.ros.hub_id, "DEFAULT_HUB_ID");
  EXPECT_EQ(config.logging.level, "INFO");
  EXPECT_TRUE(config.logging.enable_websocket_logs);
}

TEST_F(ConfigTest, MqttDefaultConfiguration) {
  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig();

  const auto & config = config_manager.getConfig();

  EXPECT_FALSE(config.mqtt.enabled);
  EXPECT_EQ(config.mqtt.broker, "tcp://localhost:1883");
  EXPECT_EQ(config.mqtt.region, "");
  EXPECT_EQ(config.mqtt.tenant_id, "");
  EXPECT_EQ(config.mqtt.username, "");
  EXPECT_EQ(config.mqtt.password, "");
  EXPECT_EQ(config.mqtt.qos, 1);
  EXPECT_EQ(config.mqtt.keep_alive_interval, 20);
  EXPECT_TRUE(config.mqtt.clean_session);
}

TEST_F(ConfigTest, LoadFromFile) {
  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig(test_config_path);

  const auto & config = config_manager.getConfig();

  EXPECT_EQ(config.websocket.port, 8080);
  EXPECT_EQ(config.websocket.host, "127.0.0.1");
  EXPECT_EQ(config.websocket.max_connections, 5);
  EXPECT_EQ(config.ros.twist_topic_queue_size, 5);
  EXPECT_EQ(config.ros.hub_id, "DEFAULT_HUB_ID");
  EXPECT_EQ(config.logging.level, "DEBUG");
  EXPECT_FALSE(config.logging.enable_websocket_logs);
}

TEST_F(ConfigTest, MqttLoadFromFile) {
  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig(test_config_path);

  const auto & config = config_manager.getConfig();

  EXPECT_TRUE(config.mqtt.enabled);
  EXPECT_EQ(config.mqtt.broker, "tcp://broker.example.com:1883");
  EXPECT_EQ(config.mqtt.region, "us-east-1");
  EXPECT_EQ(config.mqtt.tenant_id, "tenant-abc");
  EXPECT_EQ(config.mqtt.username, "user1");
  EXPECT_EQ(config.mqtt.password, "pass1");
  EXPECT_EQ(config.mqtt.qos, 2);
  EXPECT_EQ(config.mqtt.keep_alive_interval, 30);
  EXPECT_FALSE(config.mqtt.clean_session);
}

TEST_F(ConfigTest, EnvironmentVariableOverride) {
  setenv("TWIST_TOPIC_QUEUE_SIZE", "15", 1);

  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig(test_config_path);

  const auto & config = config_manager.getConfig();

  // Supported environment variables should override file values.
  EXPECT_EQ(config.ros.hub_id, "DEFAULT_HUB_ID");
  EXPECT_EQ(config.ros.twist_topic_queue_size, 15);

  // WebSocket and logging values should come from file.
  EXPECT_EQ(config.websocket.port, 8080);
  EXPECT_EQ(config.websocket.host, "127.0.0.1");
  EXPECT_EQ(config.websocket.max_connections, 5);
  EXPECT_EQ(config.logging.level, "DEBUG");
  EXPECT_FALSE(config.logging.enable_websocket_logs);

  unsetenv("TWIST_TOPIC_QUEUE_SIZE");
}

TEST_F(ConfigTest, VelocityBridgeConfigEnvironmentVariable) {
  // Create alternative config file
  std::string alt_config_content =
    R"({
        "websocket": {
            "port": 9999,
            "host": "192.168.1.100",
            "max_connections": 15
        },
        "ros": {
            "twist_topic_queue_size": 20,
            "hub_id": "ALT_HUB"
        },
        "logging": {
            "level": "WARN",
            "enable_websocket_logs": true
        },
        "mqtt": {
            "enabled": false,
            "broker": "tcp://alt-broker:1883",
            "region": "ap-south-1",
            "tenant_id": "tenant-xyz",
            "username": "",
            "password": "",
            "qos": 1,
            "keep_alive_interval": 20,
            "clean_session": true
        }
    })";

  std::string alt_config_path = "alt_test_config.json";
  writeFile(alt_config_path, alt_config_content);

  // Set REMOTE_CONTROLLER_CONFIG environment variable
  setenv("REMOTE_CONTROLLER_CONFIG", alt_config_path.c_str(), 1);

  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig();   // No explicit path - should use environment variable

  const auto & config = config_manager.getConfig();

  // Should load from alternative config file specified by environment variable
  EXPECT_EQ(config.websocket.port, 9999);
  EXPECT_EQ(config.websocket.host, "192.168.1.100");
  EXPECT_EQ(config.websocket.max_connections, 15);
  EXPECT_EQ(config.ros.twist_topic_queue_size, 20);
  EXPECT_EQ(config.ros.hub_id, "DEFAULT_HUB_ID");
  EXPECT_EQ(config.logging.level, "WARN");
  EXPECT_TRUE(config.logging.enable_websocket_logs);
  EXPECT_EQ(config.mqtt.broker, "tcp://alt-broker:1883");
  EXPECT_EQ(config.mqtt.region, "ap-south-1");
  EXPECT_EQ(config.mqtt.tenant_id, "tenant-xyz");

  // Clean up
  unsetenv("REMOTE_CONTROLLER_CONFIG");
  if (std::filesystem::exists(alt_config_path)) {
    std::filesystem::remove(alt_config_path);
  }
}

TEST_F(ConfigTest, MqttEnvironmentVariableOverride) {
  setenv("MQTT_BROKER", "tcp://env-broker:1883", 1);
  setenv("MQTT_REGION", "eu-west-1", 1);
  setenv("MQTT_TENANT_ID", "env-tenant", 1);
  setenv("MQTT_USERNAME", "env-user", 1);
  setenv("MQTT_PASSWORD", "env-pass", 1);

  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig(test_config_path);

  const auto & config = config_manager.getConfig();

  EXPECT_EQ(config.mqtt.broker, "tcp://env-broker:1883");
  EXPECT_EQ(config.mqtt.region, "eu-west-1");
  EXPECT_EQ(config.mqtt.tenant_id, "env-tenant");
  EXPECT_EQ(config.mqtt.username, "env-user");
  EXPECT_EQ(config.mqtt.password, "env-pass");
  EXPECT_TRUE(config.mqtt.enabled);
  EXPECT_EQ(config.mqtt.qos, 2);
  EXPECT_EQ(config.mqtt.keep_alive_interval, 30);
  EXPECT_FALSE(config.mqtt.clean_session);

  unsetenv("MQTT_BROKER");
  unsetenv("MQTT_REGION");
  unsetenv("MQTT_TENANT_ID");
  unsetenv("MQTT_USERNAME");
  unsetenv("MQTT_PASSWORD");
}

int main(int argc, char ** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
