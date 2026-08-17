# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
# Source ROS2 environment first
source /opt/ros/humble/setup.bash

# Full build
colcon build

# Build specific packages only
colcon build --packages-select robot_adapter_interfaces robot_switch_server adapter_go2

# Run (launches both robot_switch_server and remote_controller)
source install/setup.bash
ros2 launch robot_switch_server robot_switch_system.launch.py
# Override switch server config: config_file:=/abs/path/to/server.yaml
# Override remote_controller config: set REMOTE_CONTROLLER_CONFIG in env
```

### Optional Dependencies

- `cpp-httplib` — optional; graceful stub when missing (`ROBOT_SWITCH_HAVE_HTTP` compile flag)
- `libwebsocketpp-dev`, `nlohmann-json3-dev` — required by `remote_controller`
- `libpaho-mqtt-dev`, `libpaho-mqttpp-dev` — required by `remote_controller` when MQTT subscriber is enabled

No unit test suite exists at the workspace level. `remote_controller` has its own GTest suite for `ConfigManager` (see `src/remote_controller/CLAUDE.md`). No linter is configured.

## Architecture

Six ROS2 packages. Runtime wiring:

```
robot_adapter_interfaces  (shared types + AdapterNodeBase + AdapterClient + SystemInfoBuilder)
        ↑
robot_switch_server       (HTTP control, fork/exec adapter processes)
        ↑  (fork/exec at runtime)
adapter_go2 / adapter_lynx / adapter_fake
                                          
remote_controller         (WebSocket + MQTT → /{HUB_ID}/cmd_vel)  — sibling node, launched
                                                                    alongside robot_switch_server
```

`robot_switch_server` and `remote_controller` run as independent ROS2 nodes from the same launch file. Adapters are spawned on demand by the switch server via fork/exec.

### robot_switch_server

Central orchestrator. Manages adapter process lifecycles (at most one adapter running at a time) and exposes HTTP endpoints.

HTTP endpoints: `/health`, `/status`, `/start`, `/stop`, `/system_info`, `/adapters`.

Source layout:

- `core/AdapterProcessSupervisor` — OS-level process management: `fork()`/`exec()` with `pipe2(O_CLOEXEC)` for exec error detection, `setpgid(0,0)` + `prctl(PR_SET_PDEATHSIG, SIGKILL)` for orphan prevention, `signalfd(SIGCHLD)` reaper thread for zombie cleanup, graceful stop via `kill(-pgid, SIGTERM)` → grace timeout → `kill(-pgid, SIGKILL)`
- `core/AdapterStateMachine` — Explicit state machine: `Idle → Starting → Running → Stopping → Idle`, plus `Faulted` and `ShuttingDown`. All transitions serialized through `ProcessEvent()`
- `core/AdapterRuntimeManager` — Orchestrates adapter lifecycle (supervisor + state machine), validates requests, launches/stops adapters, manages RPC client connections. Exposes `GetEnabledAdapterTypes()` for the `/adapters` endpoint
- `http/JsonResponseBuilder` — HTTP response JSON construction (status, operation, system_info, adapters)
- `infra/HttpServerRunner` — pimpl over cpp-httplib, runs the HTTP loop in a dedicated thread
- `utils/` — `JsonBuilder`/`JsonArrayBuilder` (hand-rolled JSON), `TrimCopy`, constants

Default listen address is `0.0.0.0:9098` (see `config/server.yaml`).

### robot_adapter_interfaces

Shared types and base classes:

- Enums: `SwitchState`, `ErrorCode`
- Structs: `AdapterStatus`, `OperationResult`, `SwitchStatusSnapshot`, `MotionState`
- `AdapterNodeBase` — CRTP-free base class for adapter nodes. Owns the 5 standard services and dispatches to pure-virtual `OnConnect/OnDisconnect/OnSafeStop/OnHealth/OnSystemInfo`. Also exposes `RegisterExtensions()` hook, `LoadConfigFromFile()` (reads `<pkg_share>/config/<node_name>.yaml`), and `GetCmdVelTopic()` which returns `/{SN}/cmd_vel` (hub_id sourced from `/workspace/.info/device_info.json`)
- `AdapterClient` — ROS2 service client wrapper with a dedicated internal node + `SingleThreadedExecutor` to avoid executor conflicts with the caller's node
- `SystemInfoBuilder` — unified JSON builder for `/system_info` payloads. Accepts battery (single int or per-leg vector), motion `{x, y, yaw}`, and a raw `details_json` escape hatch

### Adapter service contract

Every adapter exposes 5 services at `/adapter_<type>/{connect,disconnect,safe_stop,health,system_info}` using `std_srvs::srv::Trigger`. Extension services may be added under the same prefix (see `adapter_lynx`).

`cmd_vel` topic is **shared across adapters**: `/{HUB_ID}/cmd_vel` (device SN namespace), not per-adapter. `remote_controller` publishes here; whichever adapter is currently connected subscribes.

### adapter_go2

Unitree Go2 adapter (bundles Unitree SDK2 locally via `ADAPTER_GO2_USE_LOCAL_UNITREE_SDK2=ON`).

- Control state machine: `Disconnected → ConnectedIdle → ConnectedCommanding → Fault`
- cmd_vel watchdog: calls `StopMove()` if no cmd_vel received within `cmd_vel_timeout_ms` while commanding
- Configurable `safe_stop_action`: `stop_move` (default), `stop_and_sit`, or `damp`
- Strict connect/disconnect: `StopMove()` failure during connect fails the operation; disconnect keeps `connected_=true` on failure so the caller can retry
- Config knobs: `network_interface`, `max_linear_x/y`, `max_angular_z`, `cmd_vel_timeout_ms`, `connect_state_timeout_ms`, `state_stale_timeout_ms`

### adapter_lynx

DEEP Robotics Lynx quadruped adapter, UDP protocol (default `10.21.31.103:30000`).

- cmd_vel watchdog identical in shape to go2 (zero-velocity send on timeout while commanding)
- Extension services under `/adapter_lynx/`: `mode/regular`, `gait/{walk,trot}`, `motion/{normal,agile}`, `lights/{on,off}`, `charge/{start,stop}`, `sleep/{enter,exit,query}`
- Sync query path: `query_timeout_ms` bounds how long to wait for a matching UDP response before returning
- Status freshness: `status_stale_timeout_ms` — health/system_info report degraded if no status push received within that window (Lynx pushes at 2 Hz)

### adapter_fake

Test stub with configurable behavior modes (`normal`, `exit_immediately`, `connect_fail`, `disconnect_hang`, `safe_stop_fail`, `delayed_sigterm`, `fork_child`) for exercising process lifecycle management.

### remote_controller

WebSocket- and MQTT-to-`cmd_vel` bridge. Publishes `geometry_msgs/Twist` to `/{HUB_ID}/cmd_vel`. **No compile-time dependency on other workspace packages** — it's a pure sibling.

- `WebSocketServerManager` — websocketpp (non-TLS), default port 9099, connection cap enforced in `onOpen()`
- `MqttSubscriberManager` — Paho MQTT C++ async client, subscribes to `sys/{region}/{tenant_id}/{HUB_ID}/remote_control/downlink`, publishes responses to `.../uplink`
- `MessageValidator` → `VelocityProcessor` → ROS2 publisher pipeline
- `ResponseBuilder` — all responses shaped as `{code, msg, data, requestId}`; code 0 = success, 1001–5002 = errors
- `ConfigManager` — three-tier: defaults → JSON file (`REMOTE_CONTROLLER_CONFIG` env) → env vars. `hub_id` is sourced only from `/workspace/.info/device_info.json` (SN field)
- Velocity limits hardcoded at init: `linear_x ∈ [-5.0, 5.0]`, `angular_z ∈ [-3.14, 3.14]`

See `src/remote_controller/CLAUDE.md` for full package-level guidance.

## Key Conventions

- C++17 standard across all packages
- `cpp-httplib` optional in `robot_switch_server` — graceful degrade via `ROBOT_SWITCH_HAVE_HTTP`
- Adapter naming: package `adapter_<type>`, executable `adapter_<type>_node`, services `/adapter_<type>/...`
- Config is YAML-driven via ROS2 parameters, loaded by `AdapterNodeBase::LoadConfigFromFile()` from `<pkg_share>/config/<node_name>.yaml`
- `enabled_adapter_types` whitelist in `server.yaml` gates which adapters `/start` will accept
- Device ID (SN) from `/workspace/.info/device_info.json` — mandatory for `cmd_vel` namespacing and MQTT topics
- Thread safety via `std::mutex` in `AdapterRuntimeManager` and adapter node state
- Process supervision: `AdapterProcessSupervisor` centralizes fork/exec/reap/kill. `SIGCHLD` is blocked before any threads are created via `BlockSigchld()` in `main()`
- State machine: `AdapterStateMachine` governs all adapter lifecycle transitions. External `SwitchState` is mapped from internal `AdapterState` via `MapToSwitchState()`
- The shared `/{HUB_ID}/cmd_vel` topic is the single integration point between `remote_controller` and whichever adapter is active
