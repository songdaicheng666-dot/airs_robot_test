# MQTT Remote Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add MQTT subscription transport to the remote controller, so velocity commands received via MQTT broker are validated and published as `cmd_vel` Twist messages, parallel to the existing WebSocket transport.

**Architecture:** New `MqttSubscriberManager` class alongside existing `WebSocketServerManager`, both feeding the same `VelocityProcessor`. Config extended with `MqttConfig` struct loaded from JSON files and env vars. Paho MQTT C++ async client for broker communication. MQTT transport is **optional at build time** — gated by `REMOTE_CONTROLLER_HAVE_PAHO_MQTT` compile flag so the package builds without Paho installed.

**Tech Stack:** Paho MQTT C++ (`mqtt::async_client`), nlohmann/json, ROS2 Humble, GTest

**Spec:** `docs/superpowers/specs/2026-04-15-mqtt-remote-control-design.md`

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `include/remote_controller/config.hpp` | Modify | Add `MqttConfig` struct, add `mqtt` field to `Config`, add `getMqttConfig()` accessor |
| `src/config.cpp` | Modify | Add MQTT defaults, JSON file loading, env var overrides for MQTT config |
| `include/remote_controller/mqtt_subscriber.hpp` | Create | `MqttSubscriberManager` class header |
| `src/mqtt_subscriber.cpp` | Create | `MqttSubscriberManager` implementation |
| `src/remote_controller.cpp` | Modify | Add `setupMqttSubscriber()`, wire MQTT into node lifecycle |
| `CMakeLists.txt` | Modify | Add PahoMqttCpp optional dependency with `REMOTE_CONTROLLER_HAVE_PAHO_MQTT` compile flag, conditionally include `mqtt_subscriber.cpp` |
| `package.xml` | Modify | Add conditional `paho-mqtt-cpp` dependency |
| `config/remote_controller_config.json` | Modify | Add `"mqtt"` section |
| `config/development_config.json` | Modify | Add `"mqtt"` section |
| `config/production_config.json` | Modify | Add `"mqtt"` section |
| `test/test_config.cpp` | Modify | Add tests for MQTT config loading |

---

### Task 1: Extend ConfigManager with MqttConfig

**Files:**
- Modify: `include/remote_controller/config.hpp`
- Modify: `src/config.cpp`
- Modify: `test/test_config.cpp`

- [ ] **Step 1: Write the failing test for MQTT config defaults**

Add a new test case to `test/test_config.cpp`. Insert this test after the existing `DefaultConfiguration` test (after line 101):

```cpp
TEST_F(ConfigTest, MqttDefaultConfiguration) {
  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig();

  const auto& config = config_manager.getConfig();

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
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
source /opt/ros/humble/setup.bash
colcon build --packages-select remote_controller
colcon test --packages-select remote_controller
colcon test-result --verbose
```

Expected: Compilation error — `MqttConfig` does not exist, `config.mqtt` does not exist.

- [ ] **Step 3: Add MqttConfig struct and Config field**

In `include/remote_controller/config.hpp`, add the `MqttConfig` struct after `LoggingConfig` (after line 28) and before the `Config` struct:

```cpp
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
```

Add the `mqtt` field to the `Config` struct so it becomes:

```cpp
struct Config
{
  WebSocketConfig websocket;
  ROSConfig ros;
  LoggingConfig logging;
  MqttConfig mqtt;
};
```

Add the accessor to `ConfigManager`'s public section (after the `getLoggingConfig` accessor on line 52):

```cpp
const MqttConfig& getMqttConfig() const { return config_.mqtt; }
```

- [ ] **Step 4: Add MQTT defaults to loadDefaults()**

In `src/config.cpp`, add MQTT defaults at the end of `loadDefaults()` (after line 78):

```cpp
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
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
colcon build --packages-select remote_controller
colcon test --packages-select remote_controller
colcon test-result --verbose
```

Expected: `MqttDefaultConfiguration` PASSES. All existing tests still pass.

- [ ] **Step 6: Write failing test for MQTT config from JSON file**

Add this test to `test/test_config.cpp` after the `MqttDefaultConfiguration` test:

```cpp
TEST_F(ConfigTest, MqttLoadFromFile) {
  // Create a config file with MQTT section
  std::string mqtt_config_content =
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

  std::string mqtt_config_path = "test_mqtt_config.json";
  std::ofstream file(mqtt_config_path);
  file << mqtt_config_content;
  file.close();

  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig(mqtt_config_path);

  const auto& config = config_manager.getConfig();

  EXPECT_TRUE(config.mqtt.enabled);
  EXPECT_EQ(config.mqtt.broker, "tcp://broker.example.com:1883");
  EXPECT_EQ(config.mqtt.region, "us-east-1");
  EXPECT_EQ(config.mqtt.tenant_id, "tenant-abc");
  EXPECT_EQ(config.mqtt.username, "user1");
  EXPECT_EQ(config.mqtt.password, "pass1");
  EXPECT_EQ(config.mqtt.qos, 2);
  EXPECT_EQ(config.mqtt.keep_alive_interval, 30);
  EXPECT_FALSE(config.mqtt.clean_session);

  std::filesystem::remove(mqtt_config_path);
}
```

- [ ] **Step 7: Run test to verify it fails**

```bash
colcon build --packages-select remote_controller && colcon test --packages-select remote_controller && colcon test-result --verbose
```

Expected: `MqttLoadFromFile` FAILS — MQTT fields not loaded from JSON (still have defaults).

- [ ] **Step 8: Add MQTT JSON file loading to overrideFromFile()**

In `src/config.cpp`, add MQTT config parsing inside `overrideFromFile()`, after the logging config block (after line 129):

```cpp
    // MQTT config
    if (j.contains("mqtt")) {
      auto& mq = j["mqtt"];
      if (mq.contains("enabled")) { config_.mqtt.enabled = mq["enabled"]; }
      if (mq.contains("broker")) { config_.mqtt.broker = mq["broker"]; }
      if (mq.contains("region")) { config_.mqtt.region = mq["region"]; }
      if (mq.contains("tenant_id")) { config_.mqtt.tenant_id = mq["tenant_id"]; }
      if (mq.contains("username")) { config_.mqtt.username = mq["username"]; }
      if (mq.contains("password")) { config_.mqtt.password = mq["password"]; }
      if (mq.contains("qos")) { config_.mqtt.qos = mq["qos"]; }
      if (mq.contains("keep_alive_interval")) { config_.mqtt.keep_alive_interval = mq["keep_alive_interval"]; }
      if (mq.contains("clean_session")) { config_.mqtt.clean_session = mq["clean_session"]; }
    }
```

- [ ] **Step 9: Run test to verify it passes**

```bash
colcon build --packages-select remote_controller && colcon test --packages-select remote_controller && colcon test-result --verbose
```

Expected: `MqttLoadFromFile` PASSES. All tests pass.

- [ ] **Step 10: Write failing test for MQTT env var overrides**

Add this test to `test/test_config.cpp`. First, add MQTT env vars to the `SetUp()`/`TearDown()` save/restore pattern — add these to `SetUp()` after line 16:

```cpp
    original_mqtt_broker_ = getEnvVarOrEmpty("MQTT_BROKER");
    original_mqtt_region_ = getEnvVarOrEmpty("MQTT_REGION");
    original_mqtt_tenant_id_ = getEnvVarOrEmpty("MQTT_TENANT_ID");
    original_mqtt_username_ = getEnvVarOrEmpty("MQTT_USERNAME");
    original_mqtt_password_ = getEnvVarOrEmpty("MQTT_PASSWORD");
```

Add to `SetUp()` clear section after line 22:

```cpp
    unsetenv("MQTT_BROKER");
    unsetenv("MQTT_REGION");
    unsetenv("MQTT_TENANT_ID");
    unsetenv("MQTT_USERNAME");
    unsetenv("MQTT_PASSWORD");
```

Add to `TearDown()` restore section after line 59:

```cpp
    restoreEnvVar("MQTT_BROKER", original_mqtt_broker_);
    restoreEnvVar("MQTT_REGION", original_mqtt_region_);
    restoreEnvVar("MQTT_TENANT_ID", original_mqtt_tenant_id_);
    restoreEnvVar("MQTT_USERNAME", original_mqtt_username_);
    restoreEnvVar("MQTT_PASSWORD", original_mqtt_password_);
```

Add the private member variables after line 85:

```cpp
  std::string original_mqtt_broker_;
  std::string original_mqtt_region_;
  std::string original_mqtt_tenant_id_;
  std::string original_mqtt_username_;
  std::string original_mqtt_password_;
```

Then add the test case:

```cpp
TEST_F(ConfigTest, MqttEnvironmentVariableOverride) {
  setenv("MQTT_BROKER", "tcp://env-broker:1883", 1);
  setenv("MQTT_REGION", "eu-west-1", 1);
  setenv("MQTT_TENANT_ID", "env-tenant", 1);
  setenv("MQTT_USERNAME", "env-user", 1);
  setenv("MQTT_PASSWORD", "env-pass", 1);

  remote_controller::ConfigManager config_manager;
  config_manager.loadConfig(test_config_path);

  const auto& config = config_manager.getConfig();

  EXPECT_EQ(config.mqtt.broker, "tcp://env-broker:1883");
  EXPECT_EQ(config.mqtt.region, "eu-west-1");
  EXPECT_EQ(config.mqtt.tenant_id, "env-tenant");
  EXPECT_EQ(config.mqtt.username, "env-user");
  EXPECT_EQ(config.mqtt.password, "env-pass");

  unsetenv("MQTT_BROKER");
  unsetenv("MQTT_REGION");
  unsetenv("MQTT_TENANT_ID");
  unsetenv("MQTT_USERNAME");
  unsetenv("MQTT_PASSWORD");
}
```

- [ ] **Step 11: Run test to verify it fails**

```bash
colcon build --packages-select remote_controller && colcon test --packages-select remote_controller && colcon test-result --verbose
```

Expected: `MqttEnvironmentVariableOverride` FAILS — env vars not read for MQTT fields.

- [ ] **Step 12: Add MQTT env var overrides to overrideFromEnvironment()**

In `src/config.cpp`, add MQTT env var loading to `overrideFromEnvironment()`, after line 89:

```cpp
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
```

- [ ] **Step 13: Run all tests to verify they pass**

```bash
colcon build --packages-select remote_controller && colcon test --packages-select remote_controller && colcon test-result --verbose
```

Expected: ALL tests pass including `MqttDefaultConfiguration`, `MqttLoadFromFile`, `MqttEnvironmentVariableOverride`.

- [ ] **Step 14: Commit**

```bash
git add include/remote_controller/config.hpp src/config.cpp test/test_config.cpp
git commit -m "feat(config): add MqttConfig struct with JSON and env var loading"
```

---

### Task 2: Update config JSON files with MQTT section

**Files:**
- Modify: `config/remote_controller_config.json`
- Modify: `config/development_config.json`
- Modify: `config/production_config.json`

- [ ] **Step 1: Add MQTT section to default config**

Replace `config/remote_controller_config.json` with:

```json
{
  "websocket": {
    "port": 9099,
    "host": "0.0.0.0",
    "max_connections": 10
  },
  "ros": {
    "twist_topic_queue_size": 10,
    "hub_id": "DEFAULT_HUB_ID"
  },
  "logging": {
    "level": "INFO",
    "enable_websocket_logs": true
  },
  "mqtt": {
    "enabled": false,
    "broker": "tcp://localhost:1883",
    "region": "",
    "tenant_id": "",
    "username": "",
    "password": "",
    "qos": 1,
    "keep_alive_interval": 20,
    "clean_session": true
  }
}
```

- [ ] **Step 2: Add MQTT section to development config**

Replace `config/development_config.json` with:

```json
{
  "websocket": {
    "port": 9099,
    "host": "127.0.0.1",
    "max_connections": 5
  },
  "ros": {
    "twist_topic_queue_size": 5,
    "hub_id": "DEV_HUB"
  },
  "logging": {
    "level": "DEBUG",
    "enable_websocket_logs": true
  },
  "mqtt": {
    "enabled": false,
    "broker": "tcp://localhost:1883",
    "region": "",
    "tenant_id": "",
    "username": "",
    "password": "",
    "qos": 1,
    "keep_alive_interval": 20,
    "clean_session": true
  }
}
```

- [ ] **Step 3: Add MQTT section to production config**

Replace `config/production_config.json` with:

```json
{
  "websocket": {
    "port": 9099,
    "host": "0.0.0.0",
    "max_connections": 5
  },
  "ros": {
    "twist_topic_queue_size": 20,
    "hub_id": "PROD_HUB"
  },
  "logging": {
    "level": "WARN",
    "enable_websocket_logs": false
  },
  "mqtt": {
    "enabled": true,
    "broker": "tcp://localhost:1883",
    "region": "",
    "tenant_id": "",
    "username": "",
    "password": "",
    "qos": 1,
    "keep_alive_interval": 20,
    "clean_session": true
  }
}
```

- [ ] **Step 4: Build and run tests to verify nothing broke**

```bash
colcon build --packages-select remote_controller && colcon test --packages-select remote_controller && colcon test-result --verbose
```

Expected: All tests pass.

- [ ] **Step 5: Commit**

```bash
git add config/remote_controller_config.json config/development_config.json config/production_config.json
git commit -m "chore(config): add mqtt section to all config profiles"
```

---

### Task 3: Add build system dependencies for Paho MQTT (optional)

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `package.xml`

- [ ] **Step 1: Add PahoMqttCpp as optional dependency in CMakeLists.txt**

In `CMakeLists.txt`, add `find_package(PahoMqttCpp QUIET)` after the `find_package(websocketpp REQUIRED)` line (after line 13), followed by the conditional compile logic:

```cmake
find_package(PahoMqttCpp QUIET)
```

Replace the existing `add_executable` block with one that conditionally includes the MQTT source:

```cmake
set(REMOTE_CONTROLLER_SOURCES
  src/remote_controller.cpp
  src/config.cpp
  src/device_info.cpp
  src/response.cpp
  src/validator.cpp
  src/velocity_processor.cpp
  src/websocket_server.cpp
)

if(PahoMqttCpp_FOUND)
  list(APPEND REMOTE_CONTROLLER_SOURCES src/mqtt_subscriber.cpp)
endif()

add_executable(remote_controller_node ${REMOTE_CONTROLLER_SOURCES})
```

After `ament_target_dependencies`, add the compile flag and conditional linking:

```cmake
if(PahoMqttCpp_FOUND)
  target_compile_definitions(remote_controller_node PRIVATE REMOTE_CONTROLLER_HAVE_PAHO_MQTT=1)
  target_link_libraries(remote_controller_node
    nlohmann_json::nlohmann_json
    websocketpp::websocketpp
    PahoMqttCpp::paho-mqttpp3
  )
  message(STATUS "PahoMqttCpp found — MQTT transport enabled")
else()
  target_compile_definitions(remote_controller_node PRIVATE REMOTE_CONTROLLER_HAVE_PAHO_MQTT=0)
  target_link_libraries(remote_controller_node
    nlohmann_json::nlohmann_json
    websocketpp::websocketpp
  )
  message(STATUS "PahoMqttCpp NOT found — MQTT transport disabled, building without MQTT support")
endif()
```

This replaces the existing standalone `target_link_libraries` block (lines 34-37).

- [ ] **Step 2: Add paho-mqtt-cpp as conditional dependency in package.xml**

In `package.xml`, add after the `<depend>websocketpp</depend>` line (line 15):

```xml
  <build_depend condition="$REMOTE_CONTROLLER_HAVE_PAHO_MQTT">paho-mqtt-cpp</build_depend>
  <exec_depend condition="$REMOTE_CONTROLLER_HAVE_PAHO_MQTT">paho-mqtt-cpp</exec_depend>
```

- [ ] **Step 3: Verify the build compiles with Paho installed**

```bash
colcon build --packages-select remote_controller
```

Expected: Build succeeds. CMake output includes "PahoMqttCpp found — MQTT transport enabled".

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt package.xml
git commit -m "build: add PahoMqttCpp as optional dependency with compile flag"
```

---

### Task 4: Implement MqttSubscriberManager

**Files:**
- Create: `include/remote_controller/mqtt_subscriber.hpp`
- Create: `src/mqtt_subscriber.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create the header file**

Create `include/remote_controller/mqtt_subscriber.hpp`:

```cpp
#ifndef REMOTE_CONTROLLER_MQTT_SUBSCRIBER_HPP
#define REMOTE_CONTROLLER_MQTT_SUBSCRIBER_HPP

#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include <mqtt/async_client.h>
#include "rclcpp/rclcpp.hpp"
#include "remote_controller/config.hpp"

namespace remote_controller
{

class MqttSubscriberManager : public virtual mqtt::callback
{
public:
  using MessageHandler = std::function<nlohmann::json(const std::string&)>;

  MqttSubscriberManager(
    std::shared_ptr<ConfigManager> config_manager,
    rclcpp::Logger logger
  );

  ~MqttSubscriberManager();

  bool start();
  void stop();
  bool isRunning() const { return running_; }

  void setMessageHandler(MessageHandler handler) { message_handler_ = handler; }

  std::string getDownlinkTopic() const;
  std::string getUplinkTopic() const;

private:
  std::shared_ptr<ConfigManager> config_manager_;
  rclcpp::Logger logger_;
  std::unique_ptr<mqtt::async_client> client_;
  std::atomic<bool> running_{false};
  MessageHandler message_handler_;

  // mqtt::callback overrides
  void connected(const std::string& cause) override;
  void connection_lost(const std::string& cause) override;
  void message_arrived(mqtt::const_message_ptr msg) override;

  void subscribeToDownlink();
};

}  // namespace remote_controller

#endif  // REMOTE_CONTROLLER_MQTT_SUBSCRIBER_HPP
```

- [ ] **Step 2: Create the implementation file**

Create `src/mqtt_subscriber.cpp`:

```cpp
#include "remote_controller/mqtt_subscriber.hpp"

namespace remote_controller
{

MqttSubscriberManager::MqttSubscriberManager(
  std::shared_ptr<ConfigManager> config_manager,
  rclcpp::Logger logger
)
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
  const auto& config = config_manager_->getConfig();
  return "sys/" + config.mqtt.region + "/" + config.mqtt.tenant_id + "/" +
         config.ros.hub_id + "/remote_control/downlink";
}

std::string MqttSubscriberManager::getUplinkTopic() const
{
  const auto& config = config_manager_->getConfig();
  return "sys/" + config.mqtt.region + "/" + config.mqtt.tenant_id + "/" +
         config.ros.hub_id + "/remote_control/uplink";
}

bool MqttSubscriberManager::start()
{
  if (running_.load()) {
    RCLCPP_WARN(logger_, "MQTT subscriber is already running");
    return false;
  }

  const auto& mqtt_config = config_manager_->getMqttConfig();
  const auto& ros_config = config_manager_->getROSConfig();

  std::string client_id = "remote_controller_" + ros_config.hub_id;

  try {
    client_ = std::make_unique<mqtt::async_client>(mqtt_config.broker, client_id);
    client_->set_callback(*this);

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
    auto tok = client_->connect(conn_opts);
    tok->wait_for(std::chrono::seconds(5));

    if (!client_->is_connected()) {
      RCLCPP_ERROR(logger_, "Failed to connect to MQTT broker: %s", mqtt_config.broker.c_str());
      return false;
    }

    // Subscribe synchronously — wait for broker confirmation before reporting success
    auto sub_tok = client_->subscribe(getDownlinkTopic(), mqtt_config.qos);
    sub_tok->wait_for(std::chrono::seconds(5));
    if (sub_tok->get_reason_code() != mqtt::ReasonCode::SUCCESS &&
        sub_tok->get_reason_code() != mqtt::ReasonCode::GRANTED_QOS_0 &&
        sub_tok->get_reason_code() != mqtt::ReasonCode::GRANTED_QOS_1 &&
        sub_tok->get_reason_code() != mqtt::ReasonCode::GRANTED_QOS_2) {
      RCLCPP_ERROR(logger_, "MQTT subscribe rejected by broker for topic: %s",
        getDownlinkTopic().c_str());
      client_->disconnect()->wait_for(std::chrono::seconds(2));
      return false;
    }

    running_.store(true);

    RCLCPP_INFO(logger_, "MQTT subscriber started. Downlink: %s", getDownlinkTopic().c_str());
    RCLCPP_INFO(logger_, "MQTT subscriber uplink topic: %s", getUplinkTopic().c_str());
    return true;

  } catch (const mqtt::exception& e) {
    RCLCPP_ERROR(logger_, "MQTT connection error: %s", e.what());
    return false;
  }
}

void MqttSubscriberManager::stop()
{
  if (!running_.load()) {
    return;
  }

  running_.store(false);

  try {
    if (client_ && client_->is_connected()) {
      client_->unsubscribe(getDownlinkTopic())->wait_for(std::chrono::seconds(2));
      client_->disconnect()->wait_for(std::chrono::seconds(2));
    }
  } catch (const mqtt::exception& e) {
    RCLCPP_WARN(logger_, "Error during MQTT disconnect: %s", e.what());
  }

  RCLCPP_INFO(logger_, "MQTT subscriber stopped");
}

void MqttSubscriberManager::subscribeToDownlink()
{
  const auto& mqtt_config = config_manager_->getMqttConfig();
  try {
    auto tok = client_->subscribe(getDownlinkTopic(), mqtt_config.qos);
    tok->wait_for(std::chrono::seconds(5));
    RCLCPP_INFO(logger_, "MQTT re-subscribed to: %s", getDownlinkTopic().c_str());
  } catch (const mqtt::exception& e) {
    RCLCPP_ERROR(logger_, "MQTT re-subscribe failed: %s", e.what());
  }
}

void MqttSubscriberManager::connected(const std::string& cause)
{
  RCLCPP_INFO(logger_, "MQTT connected. Cause: %s", cause.c_str());
  if (running_.load()) {
    subscribeToDownlink();
  }
}

void MqttSubscriberManager::connection_lost(const std::string& cause)
{
  RCLCPP_WARN(logger_, "MQTT connection lost. Cause: %s",
    cause.empty() ? "unknown" : cause.c_str());
}

void MqttSubscriberManager::message_arrived(mqtt::const_message_ptr msg)
{
  if (!message_handler_) {
    RCLCPP_ERROR(logger_, "No message handler set for MQTT subscriber");
    return;
  }

  try {
    const std::string& payload = msg->to_string();
    nlohmann::json response = message_handler_(payload);

    // Publish response to uplink topic
    const auto& mqtt_config = config_manager_->getMqttConfig();
    auto response_msg = mqtt::make_message(getUplinkTopic(), response.dump());
    response_msg->set_qos(mqtt_config.qos);
    client_->publish(response_msg);
  } catch (const mqtt::exception& e) {
    RCLCPP_WARN(logger_, "Failed to publish MQTT response: %s", e.what());
  } catch (const std::exception& e) {
    RCLCPP_ERROR(logger_, "Error handling MQTT message: %s", e.what());
  }
}

}  // namespace remote_controller
```

- [ ] **Step 3: Verify mqtt_subscriber.cpp is included by CMakeLists.txt**

Task 3 already added `src/mqtt_subscriber.cpp` to the conditional `REMOTE_CONTROLLER_SOURCES` list. No CMakeLists.txt changes needed here. Verify the file is picked up by building:

- [ ] **Step 4: Build to verify compilation**

```bash
colcon build --packages-select remote_controller
```

Expected: Build succeeds with no errors.

- [ ] **Step 5: Run existing tests to verify no regressions**

```bash
colcon test --packages-select remote_controller && colcon test-result --verbose
```

Expected: All existing tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/remote_controller/mqtt_subscriber.hpp src/mqtt_subscriber.cpp
git commit -m "feat: implement MqttSubscriberManager with synchronous subscribe"
```

---

### Task 5: Integrate MqttSubscriberManager into RemoteController node

**Files:**
- Modify: `src/remote_controller.cpp`

- [ ] **Step 1: Add the conditional include**

In `src/remote_controller.cpp`, add the MQTT header include after the websocket include (after line 11), gated by the compile flag:

```cpp
#if REMOTE_CONTROLLER_HAVE_PAHO_MQTT
#include "remote_controller/mqtt_subscriber.hpp"
#endif
```

- [ ] **Step 2: Add the member variable and method declaration**

In the `RemoteController` class, add `setupMqttSubscriber()` declaration after `setupWebSocketServer()` (after line 41):

```cpp
  void setupMqttSubscriber();
```

Add the member variable after `websocket_server_` (after line 51), gated by the compile flag:

```cpp
#if REMOTE_CONTROLLER_HAVE_PAHO_MQTT
  std::unique_ptr<remote_controller::MqttSubscriberManager> mqtt_subscriber_;
#endif
```

- [ ] **Step 3: Call setupMqttSubscriber() in constructor**

In the constructor, add `setupMqttSubscriber()` after `setupWebSocketServer()`. The constructor body becomes:

```cpp
  RemoteController()
  : Node("remote_controller")
  {
    initializeComponents();
    setupConfiguration();
    setupRosPublisher();
    setupVelocityProcessor();
    setupWebSocketServer();
    setupMqttSubscriber();

    RCLCPP_INFO(this->get_logger(), "Node ready. Listening for WebSocket commands...");
  }
```

Update the log message to reflect both transports:

```cpp
    RCLCPP_INFO(this->get_logger(), "Node ready. Listening for WebSocket and MQTT commands...");
```

- [ ] **Step 4: Add MQTT cleanup to destructor**

Update the destructor to stop MQTT alongside WebSocket:

```cpp
  ~RemoteController()
  {
#if REMOTE_CONTROLLER_HAVE_PAHO_MQTT
    if (mqtt_subscriber_) {
      mqtt_subscriber_->stop();
    }
#endif
    if (websocket_server_) {
      websocket_server_->stop();
    }
  }
```

- [ ] **Step 5: Implement setupMqttSubscriber()**

Add the implementation after `setupWebSocketServer()` (after line 143):

```cpp
void RemoteController::setupMqttSubscriber()
{
#if REMOTE_CONTROLLER_HAVE_PAHO_MQTT
  const auto& config = config_manager_->getConfig();

  if (!config.mqtt.enabled) {
    RCLCPP_INFO(this->get_logger(), "MQTT subscriber disabled in config");
    return;
  }

  mqtt_subscriber_ = std::make_unique<remote_controller::MqttSubscriberManager>(
    config_manager_,
    this->get_logger()
  );

  mqtt_subscriber_->setMessageHandler(
    [this](const std::string& payload) -> nlohmann::json {
      return handleVelocityMessage(payload);
    }
  );

  if (!mqtt_subscriber_->start()) {
    RCLCPP_ERROR(this->get_logger(),
      "Failed to start MQTT subscriber. Node will continue with WebSocket only.");
  }
#else
  RCLCPP_INFO(this->get_logger(), "MQTT transport not available (built without PahoMqttCpp)");
#endif
}
```

- [ ] **Step 6: Build to verify compilation**

```bash
colcon build --packages-select remote_controller
```

Expected: Build succeeds.

- [ ] **Step 7: Run all tests**

```bash
colcon test --packages-select remote_controller && colcon test-result --verbose
```

Expected: All tests pass.

- [ ] **Step 8: Commit**

```bash
git add src/remote_controller.cpp
git commit -m "feat: integrate MqttSubscriberManager into RemoteController node"
```

---

### Task 6: Update CLAUDE.md documentation

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Update CLAUDE.md**

Update the Architecture section to include the MQTT path. After the existing ASCII diagram (showing WebSocket flow), add the MQTT path so the diagram becomes:

```
WebSocket Client (JSON)
       |
WebSocketServerManager  ── ws server on std::thread, connection set guarded by mutex
       |  string payload via std::function callback
       v
RemoteController::handleVelocityMessage()
       |
       |  (also called by MqttSubscriberManager via same callback signature)
       |
VelocityProcessor::processVelocityCommand()
       |── MessageValidator::validateVelocityCommand()  (structure → fields → types → ranges)
       |── publisher_->publish(Twist)
       |── ResponseBuilder → JSON response
       v
JSON response back to WebSocket client (or published to MQTT uplink topic)
```

Add `MqttSubscriberManager` documentation after the `WebSocketServerManager` section:

```
**MqttSubscriberManager** (`mqtt_subscriber.hpp/.cpp`) — Paho MQTT C++ async client subscriber. Connects to configurable broker, subscribes to `sys/{region}/{tenant_id}/{device_id}/remote_control/downlink`. On message arrival, invokes same `handleVelocityMessage` callback as WebSocket. Publishes response JSON to `sys/{region}/{tenant_id}/{device_id}/remote_control/uplink`. Auto-reconnect enabled. Controlled by `mqtt.enabled` config flag.
```

Update the Build & Test section to add `libpaho-mqttpp-dev` to system dependencies:

```
System dependencies: `libwebsocketpp-dev`, `nlohmann-json3-dev`, `ros-humble-geometry-msgs`, `libpaho-mqtt-dev`, `libpaho-mqttpp-dev`.
```

Add MQTT env vars to the Key Conventions noting:

```
- MQTT config via env vars: `MQTT_BROKER`, `MQTT_REGION`, `MQTT_TENANT_ID`, `MQTT_USERNAME`, `MQTT_PASSWORD`
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md with MQTT subscriber documentation"
```

---

### Task 7: Final verification build

**Files:** None (verification only)

- [ ] **Step 1: Clean build from scratch**

```bash
cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
source /opt/ros/humble/setup.bash
rm -rf build/remote_controller install/remote_controller log/
colcon build --packages-select remote_controller
```

Expected: Clean build succeeds with no warnings related to MQTT code.

- [ ] **Step 2: Run all tests**

```bash
colcon test --packages-select remote_controller && colcon test-result --verbose
```

Expected: All tests pass — existing config tests, new MQTT config tests.

- [ ] **Step 3: Verify node starts (with MQTT disabled)**

```bash
source install/setup.bash
timeout 3 ros2 run remote_controller remote_controller_node || true
```

Expected: Node starts, logs "MQTT subscriber disabled in config", then times out after 3s. No crashes.
