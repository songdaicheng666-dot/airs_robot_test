# Remote Controller Integration Design

**Date**: 2026-03-24
**Status**: Approved

## Overview

Integrate the `remote_controller` WebSocket bridge package from `websocket-sport` into the `robot-sport` ROS2 workspace (`ros2_workspace_cpp`), and align all adapters to subscribe to a unified `/{SN}/cmd_vel` topic where SN is the device serial number from `/workspace/.info/device_info.json`.

## Goals

1. Merge `remote_controller` into `ros2_workspace_cpp/src/` so both components are built and launched together
2. Unify `cmd_vel` topic naming: all adapters subscribe to `/{SN}/cmd_vel` (e.g. `/GS20250004/cmd_vel`)
3. Enforce the convention at `AdapterNodeBase` level so all current and future adapters inherit it automatically
4. Single launch file starts everything

## Architecture

```
WebSocket Client
       │ JSON {linear_x, angular_z}
       ▼
remote_controller_node
  - DeviceInfoReader reads SN from /workspace/.info/device_info.json
  - Publishes geometry_msgs/Twist to /{SN}/cmd_vel
       │
       ▼ ROS2 topic /{SN}/cmd_vel (e.g. /GS20250004/cmd_vel)
       │
adapter_go2_node (or any future adapter)
  - AdapterNodeBase::GetCmdVelTopic() reads SN → /{SN}/cmd_vel
  - cmd_vel_sub_ subscribes to this topic
```

**Lifecycle (unchanged):** `robot_switch_server` manages adapter process lifecycle via HTTP (`/start`, `/stop`, `/status`). `remote_controller` runs independently and always publishes — it has no awareness of adapter state.

## Changes

### 1. Copy `remote_controller` package

Copy `websocket-sport/src/remote_controller/` → `ros2_workspace_cpp/src/remote_controller/`.
No source code changes. The package already reads SN via `DeviceInfoReader` and publishes to `/{SN}/cmd_vel`.

**Note on config path at runtime:** `ConfigManager` looks for the config file at `REMOTE_CONTROLLER_CONFIG` env var first, then falls back to a relative path `config/remote_controller_config.json`. When installed via colcon, this relative path will not resolve. Either set `REMOTE_CONTROLLER_CONFIG` to the installed share path, or configure it via the launch file using `FindPackageShare`.

### 2. Fix `remote_controller/CMakeLists.txt`

Move `find_package(GTest REQUIRED)` from top-level into the `if(BUILD_TESTING)` block. The `ament_cmake_gtest` package (already inside the block on line 52) covers the GTest symbols needed by the test target — the top-level `find_package(GTest REQUIRED)` is redundant and will fail builds when GTest is absent and testing is disabled.

Before:
```cmake
find_package(GTest REQUIRED)    # ← remove from here
...
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ...
endif()
```

After:
```cmake
if(BUILD_TESTING)
  find_package(GTest REQUIRED)  # ← moved here
  find_package(ament_cmake_gtest REQUIRED)
  ...
endif()
```

### 3. Add `GetCmdVelTopic()` to `AdapterNodeBase`

**No new CMake package dependencies needed.** SN is read using standard C++ `<fstream>` + string search — the device_info.json format is fixed and simple enough that no JSON library is required in the shared library. `<fstream>` must be added to the include block of `adapter_node_base.cpp`.

**`robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_node_base.hpp`** — header additions:

```cpp
// In protected: section
protected:
    // ...existing virtual methods...
    std::string GetCmdVelTopic() const;

// In private: section
private:
    // ...existing members...
    std::string hub_id_;  // SN from device_info.json; empty if unreadable
```

**`robot_adapter_interfaces/src/adapter_node_base.cpp`** — implementation:

Add `#include <fstream>` to the include block, then add the following. `ReadDeviceSN` must be defined **before** the constructor (as a file-local static function) so the constructor can call it:

```cpp
// Add to includes:
#include <fstream>

// File-local helper — place BEFORE the constructor definition, in anonymous namespace:
namespace {
std::string ReadDeviceSN(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string content((std::istreambuf_iterator<char>(f)), {});
    const std::string key = "\"SN\"";
    size_t key_pos = content.find(key);
    if (key_pos == std::string::npos) return "";
    size_t colon = content.find(':', key_pos + key.size());
    if (colon == std::string::npos) return "";
    size_t open_q = content.find('"', colon + 1);
    if (open_q == std::string::npos) return "";
    size_t close_q = content.find('"', open_q + 1);
    if (close_q == std::string::npos) return "";
    return content.substr(open_q + 1, close_q - open_q - 1);
}
}  // namespace

// In constructor body, after existing init:
hub_id_ = ReadDeviceSN("/workspace/.info/device_info.json");
if (hub_id_.empty()) {
    RCLCPP_WARN(get_logger(),
        "SN not found in device_info.json; cmd_vel topic falls back to /%s/cmd_vel",
        get_name());
}

// GetCmdVelTopic implementation:
std::string AdapterNodeBase::GetCmdVelTopic() const {
    const std::string prefix = hub_id_.empty() ? get_name() : hub_id_;
    return "/" + prefix + "/cmd_vel";
    // Concrete examples:
    //   SN present:  /GS20250004/cmd_vel
    //   SN absent:   /adapter_go2/cmd_vel  (backward-compatible fallback)
}
```

No changes to `robot_adapter_interfaces/CMakeLists.txt` or `package.xml`.

### 4. Update `adapter_go2` cmd_vel subscription

In `Go2AdapterNode::RegisterExtensions()`, replace only the `cmd_vel` subscription. Services continue using the existing `prefix` variable unchanged.

Before:
```cpp
void Go2AdapterNode::RegisterExtensions() {
    const std::string prefix = "/" + std::string(get_name()) + "/";
    // ... four service registrations using prefix ...
    cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
        prefix + "cmd_vel", 10, ...);
```

After:
```cpp
void Go2AdapterNode::RegisterExtensions() {
    const std::string prefix = "/" + std::string(get_name()) + "/";
    // ... four service registrations using prefix — unchanged ...
    cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
        GetCmdVelTopic(), 10, ...);
```

`adapter_fake` and `adapter_m20pro` have no `cmd_vel` subscription — no changes needed.

### 5. Update launch file

`robot_switch_server/launch/robot_switch_system.launch.py` — add `remote_controller_node`. `FindPackageShare` is already imported. Add `remote_controller_config` and `remote_ctrl`, and include both in the returned `LaunchDescription`:

```python
remote_controller_config = PathJoinSubstitution(
    [FindPackageShare("remote_controller"), "config", "remote_controller_config.json"]
)

remote_ctrl = Node(
    package="remote_controller",
    executable="remote_controller_node",
    output="screen",
    additional_env={"REMOTE_CONTROLLER_CONFIG": remote_controller_config},
)

return LaunchDescription(
    [
        config_file,
        switch_server,
        remote_ctrl,
    ]
)
```

## Files Changed

| File | Change |
|------|--------|
| `src/remote_controller/` | New (copied from websocket-sport) |
| `src/remote_controller/CMakeLists.txt` | Move `find_package(GTest REQUIRED)` inside `if(BUILD_TESTING)` |
| `src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_node_base.hpp` | Add `GetCmdVelTopic()` to protected, `hub_id_` to private |
| `src/robot_adapter_interfaces/src/adapter_node_base.cpp` | Read SN in constructor, add `ReadDeviceSN()` free function, implement `GetCmdVelTopic()` |
| `src/adapter_go2/src/go2_adapter_node.cpp` | Use `GetCmdVelTopic()` for cmd_vel subscription |
| `src/robot_switch_server/launch/robot_switch_system.launch.py` | Add remote_controller_node with config path env var |

## Non-Goals

- No change to adapter service topics (`/adapter_go2/connect` etc.)
- No lifecycle coupling between remote_controller and adapters
- No refactoring of remote_controller internals
- No changes to adapter_fake or adapter_m20pro
- No new dependencies in robot_adapter_interfaces
