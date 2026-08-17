# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
source /opt/ros/humble/setup.bash

# Build this package only
colcon build --packages-select remote_controller

# Run the node
source install/setup.bash
ros2 run remote_controller remote_controller_node

# Run C++ unit tests (GTest, tests ConfigManager only)
colcon test --packages-select remote_controller
colcon test-result --verbose

# Python integration tests (requires node running + websocket-client pip package)
python3 test/test_responses.py        # automated validation + interactive keyboard control
python3 test/test.py                  # load test: 20 Hz for 10 seconds
```

System dependencies: `libwebsocketpp-dev`, `nlohmann-json3-dev`, `ros-humble-geometry-msgs`.

No linter is configured. Copyright and cpplint checks are explicitly skipped in CMakeLists.txt.

## Architecture

WebSocket-to-cmd_vel bridge: accepts JSON velocity commands over WebSocket, validates them, publishes `geometry_msgs/Twist` to `/{HUB_ID}/cmd_vel`.

```
WebSocket Client (JSON)
       |
WebSocketServerManager  ── ws server on std::thread, connection set guarded by mutex
       |  string payload via std::function callback
       v
RemoteController::handleVelocityMessage()
       |
VelocityProcessor::processVelocityCommand()
       |── MessageValidator::validateVelocityCommand()  (structure → fields → types → ranges)
       |── publisher_->publish(Twist)
       |── ResponseBuilder → JSON response
       v
JSON response back to WebSocket client
```

**RemoteController** (`src/remote_controller.cpp`) is the ROS2 node. It owns all components and wires them together in `initializeComponents()` / `setup*()` methods. It is not in the `remote_controller` namespace (the library classes are).

**ConfigManager** (`config.hpp/.cpp`) — three-tier config: defaults → JSON file → env vars. Loaded via `loadConfig()`. Shared as `shared_ptr` into other components.

**hub_id is special**: sourced exclusively from `/workspace/.info/device_info.json` (the `SN` field) via `DeviceInfoReader`. It is NOT read from env vars or config files despite what the README suggests. The only env var override that works is `TWIST_TOPIC_QUEUE_SIZE`. Config file path is set via `REMOTE_CONTROLLER_CONFIG` env var.

**WebSocketServerManager** (`websocket_server.hpp/.cpp`) — `websocketpp::config::asio` (non-TLS), `set_reuse_addr(true)`. Connection count enforced in `onOpen()` against `max_connections`; excess connections closed with `policy_violation`. Server thread is a `std::thread`; running state is `std::atomic<bool>`.

**MessageValidator** (`validator.hpp/.cpp`) — validation pipeline: JSON parse → structure check → required fields (`linear_x`, `angular_z`) → type check → value range check. Short-circuits on structural errors. Optional fields: `linear_y`, `linear_z`, `angular_x`, `angular_y` (default 0.0).

**VelocityProcessor** (`velocity_processor.hpp/.cpp`) — orchestrates validation, Twist construction, publishing, and response building. Returns `ProcessingResult` (success bool + JSON response).

**ResponseBuilder** (`response.hpp/.cpp`) — all responses follow `{"code": int, "msg": string, "data": object, "requestId": string}`. Code 0 = success; error codes 1001-5002.

## Key Conventions

- All library code in `namespace remote_controller`; the node class `RemoteController` is in global namespace
- Velocity limits hardcoded in `initializeComponents()`: `linear_x` in [-5.0, 5.0] m/s, `angular_z` in [-3.14, 3.14] rad/s
- `ConfigManager` and `MessageValidator` passed as `shared_ptr`; `VelocityProcessor` and `WebSocketServerManager` owned as `unique_ptr`
- Default WebSocket port: 9099, host: 0.0.0.0
- Three config profiles in `config/`: default, development (localhost, DEBUG), production (WARN, ws logs off)
- No launch file in this package — launched by `robot_switch_server`'s `robot_switch_system.launch.py` which passes `REMOTE_CONTROLLER_CONFIG` via `additional_env`
- No compile-time dependency on other workspace packages; runtime sibling of adapter packages that consume the `cmd_vel` topic

## Known Issues

- `test/test.html` hardcodes `ws://localhost:9090` (should be 9099)
- `test/test_config.cpp` `EnvironmentVariableOverride` test sets `HUB_ID` env var but `config.cpp` does not read `HUB_ID` from env — test expectation is stale
- No explicit `CMAKE_CXX_STANDARD 17` in CMakeLists.txt despite using `std::filesystem` and `std::optional`
