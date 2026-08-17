# MQTT Remote Control Design

**Date:** 2026-04-15
**Status:** Approved
**Approach:** Parallel MqttSubscriberManager alongside existing WebSocketServerManager

## Overview

Add MQTT-based remote control to the existing WebSocket remote controller. Users publish velocity JSON messages to an MQTT broker topic; the system subscribes, validates, publishes `geometry_msgs/Twist` to `cmd_vel`, and publishes a response back to an uplink topic. The MQTT path reuses the same `VelocityProcessor` pipeline as WebSocket.

## Architecture

```
WebSocket Client ──> WebSocketServerManager ──┐
                                              ├──> VelocityProcessor ──> publish Twist
MQTT Broker      ──> MqttSubscriberManager  ──┘         |
      ^                                                 |
      └──── uplink response (JSON) <────────────────────┘
```

Both transports feed into the same `VelocityProcessor` independently. They run concurrently; the ROS2 publisher is thread-safe.

## Config Extension

### New `MqttConfig` struct

Added to `config.hpp` alongside existing `WebSocketConfig`, `ROSConfig`, `LoggingConfig`:

```cpp
struct MqttConfig {
    bool enabled{false};                          // master switch
    std::string broker{"tcp://localhost:1883"};    // broker URI
    std::string region;                            // topic hierarchy segment
    std::string tenant_id;                         // topic hierarchy segment
    std::string username;                          // optional auth (empty = anonymous)
    std::string password;                          // optional auth
    int qos{1};                                    // subscribe/publish QoS level
    int keep_alive_interval{20};                   // seconds
    bool clean_session{true};
};
```

The `Config` struct gets a new `MqttConfig mqtt` field.

### Config loading

Three-tier loading (same pattern as existing config):
1. Hardcoded defaults (struct initializers above)
2. JSON config file: `"mqtt"` key at the top level
3. Environment variable overrides: `MQTT_BROKER`, `MQTT_REGION`, `MQTT_TENANT_ID`, `MQTT_USERNAME`, `MQTT_PASSWORD`

### MQTT topic construction

Uses `device_id` (the `hub_id` / SN from `DeviceInfoReader`, same as used for `cmd_vel` topic):

- **Subscribe (downlink):** `sys/{region}/{tenant_id}/{device_id}/remote_control/downlink`
- **Publish (uplink):** `sys/{region}/{tenant_id}/{device_id}/remote_control/uplink`

The `remote_control` segment distinguishes velocity commands from other device message types.

### Config file updates

All 3 config profiles get a new `"mqtt"` section:

| Profile | `enabled` | `broker` |
|---|---|---|
| `remote_controller_config.json` (default) | `false` | `tcp://localhost:1883` |
| `development_config.json` | `false` | `tcp://localhost:1883` |
| `production_config.json` | `true` | `tcp://localhost:1883` |

`region` and `tenant_id` are left as empty strings in config files — must be provided per deployment or via env vars.

## MqttSubscriberManager

New class in `namespace remote_controller`. Files: `include/remote_controller/mqtt_subscriber.hpp`, `src/mqtt_subscriber.cpp`.

### Public interface

```cpp
class MqttSubscriberManager {
public:
    using MessageHandler = std::function<nlohmann::json(const std::string&)>;

    MqttSubscriberManager(std::shared_ptr<ConfigManager> config, rclcpp::Logger logger);
    ~MqttSubscriberManager();

    bool start();                                   // connect to broker + subscribe (synchronous)
    void stop();                                    // unsubscribe + disconnect
    bool isRunning() const;
    void setMessageHandler(MessageHandler handler);
    std::string getDownlinkTopic() const;
    std::string getUplinkTopic() const;
};
```

**Note:** The `MessageHandler` returns `nlohmann::json`, matching the existing `WebSocketServerManager` callback type and `RemoteController::handleVelocityMessage()` return type. This avoids an unnecessary serialize/parse round-trip.

### Internal behavior

- Owns a `mqtt::async_client` (Paho C++ async client)
- Client ID: `remote_controller_{device_id}` for uniqueness
- `start()`:
  1. Build connect options with optional username/password, `keep_alive_interval`, `clean_session`, `automatic_reconnect(true)`
  2. Connect synchronously (wait with timeout)
  3. Subscribe to downlink topic at configured QoS — **await the subscribe token synchronously** so `start()` only returns `true` when the subscription is confirmed by the broker. If the broker rejects the subscription (e.g. ACL, invalid topic), `start()` returns `false`.
  4. Set `running_` atomic to true
- On message arrival (via Paho callback):
  1. Extract payload string
  2. Call `message_handler_(payload)` to get response JSON
  3. Publish response to uplink topic at configured QoS
- `stop()`: unsubscribe, disconnect, set `running_` to false
- No separate `std::thread` needed — Paho async client manages its own internal thread
- `isRunning()` backed by `std::atomic<bool>`

### MessageHandler signature

Same as `WebSocketServerManager`: `std::function<nlohmann::json(const std::string&)>`. Takes a JSON payload string, returns a `nlohmann::json` response object. This matches the existing `RemoteController::handleVelocityMessage()` return type exactly, so both transports reuse the same callback without any serialize/parse overhead.

## Integration into RemoteController

### Initialization sequence

```
initializeComponents()
setupConfiguration()
setupRosPublisher()
setupVelocityProcessor()
setupWebSocketServer()      // existing
setupMqttSubscriber()       // NEW
```

### setupMqttSubscriber()

1. Check `config.mqtt.enabled` — if false, log info and return
2. Create `MqttSubscriberManager` with config + logger
3. Set message handler to `handleVelocityMessage()` (same as WebSocket)
4. Call `start()` — if fails, log error but don't crash the node

### New member

```cpp
std::unique_ptr<remote_controller::MqttSubscriberManager> mqtt_subscriber_;
```

Same ownership pattern as the existing `ws_server_`.

### Shutdown

Destructor calls `mqtt_subscriber_->stop()` alongside the existing `ws_server_->stop()`.

## Message Flow (MQTT Path)

```
MQTT client publishes JSON to sys/{region}/{tenant_id}/{device_id}/remote_control/downlink
         |
         v
Paho async_client invokes message callback
         |
         v
message_handler_(payload)       [bound to RemoteController::handleVelocityMessage]
         |
         v
velocity_processor_->processVelocityCommand(payload)
    |
    +-- parse JSON
    +-- validate (same MessageValidator pipeline)
    +-- create Twist message
    +-- publisher_->publish(twist)
    +-- build response JSON
         |
         v
Publish response JSON to sys/{region}/{tenant_id}/{device_id}/remote_control/uplink
```

### Message format

Identical to the existing WebSocket format:

```json
{
  "linear_x": 0.5,
  "angular_z": 0.1
}
```

Optional fields (default 0.0): `linear_y`, `linear_z`, `angular_x`, `angular_y`.

Response format unchanged:
```json
{
  "code": 0,
  "msg": "Velocity command processed successfully",
  "requestId": "req_XXXXXX",
  "data": { ... }
}
```

## Error Handling

### Connection failures

- If MQTT is enabled but broker is unreachable at startup, `start()` logs an error and returns `false`. The node continues running with WebSocket only. MQTT failure is non-fatal.
- Paho's `automatic_reconnect(true)` handles transient disconnections. On reconnect, the subscription is re-established.

### Message processing errors

- Invalid JSON or validation failures are handled by `VelocityProcessor`, which returns an error response JSON. This error response is published to the uplink topic.

### Uplink publish failures

- If publishing the response to the uplink topic fails, log a warning. The velocity command was already processed (or rejected). Response publishing is best-effort.

## Build System Changes

### CMakeLists.txt

- Add `find_package(PahoMqttCpp QUIET)` — **optional** build dependency, matching the workspace convention
- If found: set `REMOTE_CONTROLLER_HAVE_PAHO_MQTT=1` compile definition, add `src/mqtt_subscriber.cpp` to sources, link `PahoMqttCpp::paho-mqttpp3`
- If not found: set `REMOTE_CONTROLLER_HAVE_PAHO_MQTT=0` compile definition, MQTT source excluded. Build succeeds without Paho installed.
- All MQTT code in `remote_controller.cpp` gated by `#if REMOTE_CONTROLLER_HAVE_PAHO_MQTT`

### package.xml

- Add `<build_depend condition="$REMOTE_CONTROLLER_HAVE_PAHO_MQTT">paho-mqtt-cpp</build_depend>`
- Add `<exec_depend condition="$REMOTE_CONTROLLER_HAVE_PAHO_MQTT">paho-mqtt-cpp</exec_depend>`

This ensures `colcon build` for the full workspace does not regress on machines without Paho installed.

### New files

| File | Purpose |
|---|---|
| `include/remote_controller/mqtt_subscriber.hpp` | MqttSubscriberManager header |
| `src/mqtt_subscriber.cpp` | MqttSubscriberManager implementation |

### Testing

- Add GTest file `test/test_mqtt_config.cpp` to verify config loading of MQTT fields
- Python integration test updates to `test/test_responses.py` are out of scope for initial implementation (would require a running MQTT broker in the test environment)

## Scope Exclusions

- TLS/mTLS broker connections (plain TCP + optional username/password only)
- MQTT v5 features (using MQTT v3.1.1 via Paho defaults)
- Rate limiting on MQTT messages
- Message deduplication
- Persistent MQTT sessions across restarts
