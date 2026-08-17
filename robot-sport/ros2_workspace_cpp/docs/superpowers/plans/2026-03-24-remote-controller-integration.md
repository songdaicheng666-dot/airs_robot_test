# Remote Controller Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Merge the `remote_controller` WebSocket bridge into the `robot-sport` ROS2 workspace and align all adapters to subscribe to `/{SN}/cmd_vel` via a shared `GetCmdVelTopic()` method on `AdapterNodeBase`.

**Architecture:** `remote_controller` reads the device SN from `/workspace/.info/device_info.json` and publishes Twist to `/{SN}/cmd_vel`. `AdapterNodeBase` gains a `GetCmdVelTopic()` method that reads the same SN so all adapters — current and future — subscribe to the matching topic automatically. A single launch file starts both `robot_switch_server` and `remote_controller_node`.

**Tech Stack:** ROS2 Humble, C++17, colcon, websocketpp (`libwebsocketpp-dev`), nlohmann-json (`nlohmann-json3-dev`), Python launch files.

**Spec:** `docs/superpowers/specs/2026-03-24-remote-controller-integration-design.md`

**Working directory for all commands:** `/home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp`

---

## File Map

| Action | File |
|--------|------|
| Modify | `src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_node_base.hpp` |
| Modify | `src/robot_adapter_interfaces/src/adapter_node_base.cpp` |
| Modify | `src/adapter_go2/src/go2_adapter_node.cpp` |
| Create | `src/remote_controller/` (copy from websocket-sport) |
| Modify | `src/remote_controller/CMakeLists.txt` |
| Modify | `src/robot_switch_server/launch/robot_switch_system.launch.py` |

---

## Task 1: Add `GetCmdVelTopic()` to `AdapterNodeBase`

**Files:**
- Modify: `src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_node_base.hpp`
- Modify: `src/robot_adapter_interfaces/src/adapter_node_base.cpp`

---

- [ ] **Step 1.1: Declare `GetCmdVelTopic()` and `hub_id_` in the header**

  Open `src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_node_base.hpp`.

  In the `protected:` section (after the existing virtual methods and before the closing brace), add:
  ```cpp
  std::string GetCmdVelTopic() const;
  ```

  In the `private:` section (before the four service `SharedPtr` members), add:
  ```cpp
  std::string hub_id_;
  ```

  Result — the `private:` section should look like (showing all existing members for accurate anchoring):
  ```cpp
  private:
      std::string adapter_type_;
      std::string package_name_;
      bool initialized_{false};
      std::string hub_id_;                  // ← new, after initialized_, before config_overrides_
      std::unordered_map<std::string, rclcpp::Parameter> config_overrides_;

      rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr connect_srv_;
      rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr disconnect_srv_;
      rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr safe_stop_srv_;
      rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr health_srv_;
      rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr system_info_srv_;
  ```

---

- [ ] **Step 1.2: Add `#include <fstream>` to `adapter_node_base.cpp`**

  Open `src/robot_adapter_interfaces/src/adapter_node_base.cpp`.

  The current include block (lines 1–8) is:
  ```cpp
  #include "robot_adapter_interfaces/adapter_node_base.hpp"

  #include <filesystem>
  #include <string>

  #include <ament_index_cpp/get_package_share_directory.hpp>
  #include <rcl_yaml_param_parser/parser.h>
  ```

  Add `#include <fstream>` after `<filesystem>`:
  ```cpp
  #include "robot_adapter_interfaces/adapter_node_base.hpp"

  #include <filesystem>
  #include <fstream>
  #include <string>

  #include <ament_index_cpp/get_package_share_directory.hpp>
  #include <rcl_yaml_param_parser/parser.h>
  ```

---

- [ ] **Step 1.3: Add `ReadDeviceSN()` free function before the constructor**

  Still in `adapter_node_base.cpp`, immediately after the `#include` block and before `namespace robot_adapter_interfaces {`, add the file-local helper. It **must** appear before the constructor that calls it:

  ```cpp
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
  ```

---

- [ ] **Step 1.4: Read SN at the end of the constructor**

  In `AdapterNodeBase::AdapterNodeBase(...)`, add the following at the end of the constructor body, immediately before the closing `}` at line 46 (after the `system_info_srv_` block):

  ```cpp
      hub_id_ = ReadDeviceSN("/workspace/.info/device_info.json");
      if (hub_id_.empty()) {
          RCLCPP_WARN(
              get_logger(),
              "SN not found in device_info.json; cmd_vel topic falls back to /%s/cmd_vel",
              get_name());
      } else {
          RCLCPP_INFO(get_logger(), "cmd_vel topic: /%s/cmd_vel", hub_id_.c_str());
      }
  ```

---

- [ ] **Step 1.5: Implement `GetCmdVelTopic()`**

  After the closing `}` of `AdapterNodeBase::Init()` (currently around line 54), add:

  ```cpp
  std::string AdapterNodeBase::GetCmdVelTopic() const {
      const std::string prefix = hub_id_.empty() ? get_name() : hub_id_;
      return "/" + prefix + "/cmd_vel";
  }
  ```

---

- [ ] **Step 1.6: Build `robot_adapter_interfaces` and verify it compiles**

  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  source /opt/ros/humble/setup.bash
  colcon build --packages-select robot_adapter_interfaces
  ```

  Expected: `Finished <<< robot_adapter_interfaces` with no errors or warnings about missing symbols.

---

- [ ] **Step 1.7: Commit**

  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  git add src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_node_base.hpp \
          src/robot_adapter_interfaces/src/adapter_node_base.cpp
  git commit -m "feat(adapter): add GetCmdVelTopic() to AdapterNodeBase, reads SN from device_info.json"
  ```

---

## Task 2: Update `adapter_go2` cmd_vel subscription

**Files:**
- Modify: `src/adapter_go2/src/go2_adapter_node.cpp:67-70`

---

- [ ] **Step 2.1: Replace `prefix + "cmd_vel"` with `GetCmdVelTopic()`**

  In `Go2AdapterNode::RegisterExtensions()` (around line 67), change:

  ```cpp
  cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
      prefix + "cmd_vel",
      10,
      [this](const geometry_msgs::msg::Twist::SharedPtr message) { OnCmdVel(message); });
  ```

  To:

  ```cpp
  cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
      GetCmdVelTopic(),
      10,
      [this](const geometry_msgs::msg::Twist::SharedPtr message) { OnCmdVel(message); });
  ```

  The `prefix` variable and all four service registrations above it are unchanged.

---

- [ ] **Step 2.2: Build `adapter_go2` and verify**

  `robot_adapter_interfaces` was already built in Task 1. If running this task in isolation, build both:
  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  source /opt/ros/humble/setup.bash
  colcon build --packages-select robot_adapter_interfaces adapter_go2
  ```

  Expected: `Finished <<< robot_adapter_interfaces` and `Finished <<< adapter_go2` with no errors.

---

- [ ] **Step 2.3: Commit**

  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  git add src/adapter_go2/src/go2_adapter_node.cpp
  git commit -m "feat(adapter_go2): subscribe to /{SN}/cmd_vel via GetCmdVelTopic()"
  ```

---

## Task 3: Copy `remote_controller` package and fix its CMakeLists

**Files:**
- Create: `src/remote_controller/` (full directory copy)
- Modify: `src/remote_controller/CMakeLists.txt`

---

- [ ] **Step 3.1: Verify websocketpp and nlohmann-json are installed**

  ```bash
  dpkg -l | grep -E "libwebsocketpp-dev|nlohmann-json3-dev"
  ```

  Expected: both packages listed. If missing:
  ```bash
  sudo apt-get install libwebsocketpp-dev nlohmann-json3-dev
  ```

---

- [ ] **Step 3.2: Copy the remote_controller package**

  ```bash
  cp -r /home/zhangyuhan/workspace/production/v2/websocket-sport/src/remote_controller \
        /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp/src/remote_controller
  ```

  Verify:
  ```bash
  ls /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp/src/remote_controller/
  ```

  Expected: `CMakeLists.txt  config/  include/  package.xml  src/  test/  API.md  ...`

---

- [ ] **Step 3.3: Fix `find_package(GTest REQUIRED)` placement in CMakeLists**

  Open `src/remote_controller/CMakeLists.txt`. The current top-level section has:
  ```cmake
  find_package(ament_cmake REQUIRED)
  find_package(rclcpp REQUIRED)
  find_package(geometry_msgs REQUIRED)
  find_package(nlohmann_json REQUIRED)
  find_package(websocketpp REQUIRED)
  find_package(GTest REQUIRED)         # ← REMOVE from here
  ```

  Remove the top-level `find_package(GTest REQUIRED)` line.

  Then in the `if(BUILD_TESTING)` block, add it before `find_package(ament_cmake_gtest REQUIRED)`:
  ```cmake
  if(BUILD_TESTING)
    find_package(GTest REQUIRED)             # ← ADD here
    find_package(ament_lint_auto REQUIRED)
    find_package(ament_cmake_gtest REQUIRED)
    ...
  endif()
  ```

---

- [ ] **Step 3.4: Build `remote_controller` and verify**

  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  source /opt/ros/humble/setup.bash
  colcon build --packages-select remote_controller
  ```

  Expected: `Finished <<< remote_controller` with no errors.

---

- [ ] **Step 3.5: Commit**

  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  git add src/remote_controller/
  git commit -m "feat: add remote_controller package (websocket-to-cmd_vel bridge)"
  ```

---

## Task 4: Update launch file to start `remote_controller_node`

**Files:**
- Modify: `src/robot_switch_server/launch/robot_switch_system.launch.py`

---

- [ ] **Step 4.1: Add `remote_controller_node` to the launch description**

  Open `src/robot_switch_server/launch/robot_switch_system.launch.py`.

  The current file is:
  ```python
  from launch import LaunchDescription
  from launch.actions import DeclareLaunchArgument
  from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
  from launch_ros.actions import Node
  from launch_ros.substitutions import FindPackageShare


  def generate_launch_description() -> LaunchDescription:
      default_config = PathJoinSubstitution(
          [FindPackageShare("robot_switch_server"), "config", "server.yaml"]
      )
      config_file = DeclareLaunchArgument(
          "config_file",
          default_value=default_config,
      )

      switch_server = Node(
          package="robot_switch_server",
          executable="robot_switch_server_node",
          output="screen",
          parameters=[LaunchConfiguration("config_file")],
      )

      return LaunchDescription(
          [
              config_file,
              switch_server,
          ]
      )
  ```

  Replace the entire file with:
  ```python
  from launch import LaunchDescription
  from launch.actions import DeclareLaunchArgument
  from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
  from launch_ros.actions import Node
  from launch_ros.substitutions import FindPackageShare


  def generate_launch_description() -> LaunchDescription:
      default_config = PathJoinSubstitution(
          [FindPackageShare("robot_switch_server"), "config", "server.yaml"]
      )
      config_file = DeclareLaunchArgument(
          "config_file",
          default_value=default_config,
      )

      switch_server = Node(
          package="robot_switch_server",
          executable="robot_switch_server_node",
          output="screen",
          parameters=[LaunchConfiguration("config_file")],
      )

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

  Note: use `additional_env` (not `env`) so existing process environment variables are preserved.

---

- [ ] **Step 4.2: Build `robot_switch_server` (validates launch file Python syntax via colcon)**

  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  source /opt/ros/humble/setup.bash
  colcon build --packages-select robot_switch_server
  ```

  Expected: `Finished <<< robot_switch_server` with no errors.

---

- [ ] **Step 4.3: Commit**

  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  git add src/robot_switch_server/launch/robot_switch_system.launch.py
  git commit -m "feat(launch): start remote_controller_node alongside robot_switch_server"
  ```

---

## Task 5: Full build and smoke test

---

- [ ] **Step 5.1: Full workspace build**

  ```bash
  cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
  source /opt/ros/humble/setup.bash
  colcon build
  ```

  Expected: all packages build successfully, no errors.

---

- [ ] **Step 5.2: Verify `GetCmdVelTopic()` output via adapter startup log**

  In terminal 1 — start the system:
  ```bash
  source /opt/ros/humble/setup.bash
  source install/setup.bash
  ros2 launch robot_switch_server robot_switch_system.launch.py
  ```

  Expected log lines from `remote_controller`:
  ```
  [remote_controller]: [Config] HUB_ID: GS20250004
  [remote_controller]: Node ready. Listening for WebSocket commands...
  ```

  Expected log line from an adapter once started (via `/start` HTTP call):
  ```
  [adapter_go2]: cmd_vel topic: /GS20250004/cmd_vel
  ```

---

- [ ] **Step 5.3: Verify topic alignment**

  In terminal 2 (after starting an adapter via `curl -X POST http://localhost:8080/start -d '{"adapter_type":"go2"}'`):
  ```bash
  source /opt/ros/humble/setup.bash
  source install/setup.bash
  ros2 topic list | grep cmd_vel
  ```

  Expected:
  ```
  /GS20250004/cmd_vel
  ```

  Both `remote_controller` (publisher) and `adapter_go2` (subscriber) should appear on this single topic:
  ```bash
  ros2 topic info /GS20250004/cmd_vel
  ```

  Expected:
  ```
  Type: geometry_msgs/msg/Twist
  Publisher count: 1
  Subscription count: 1
  ```

---

- [ ] **Step 5.4: Verify WebSocket → cmd_vel pipeline end-to-end**

  In terminal 3:
  ```bash
  ros2 topic echo /GS20250004/cmd_vel
  ```

  In terminal 4 — send a test command via Python:
  ```bash
  python3 -c "
  import websocket, json
  ws = websocket.WebSocket()
  ws.connect('ws://localhost:9099')
  ws.send(json.dumps({'linear_x': 0.1, 'angular_z': 0.0}))
  print(ws.recv())
  ws.close()
  "
  ```

  Expected in terminal 4: JSON response with `"code": 0`
  Expected in terminal 3: a Twist message with `linear.x: 0.1`

---

- [ ] **Step 5.5: Send zero-velocity stop command and verify clean shutdown**

  ```bash
  python3 -c "
  import websocket, json
  ws = websocket.WebSocket()
  ws.connect('ws://localhost:9099')
  ws.send(json.dumps({'linear_x': 0.0, 'angular_z': 0.0}))
  print(ws.recv())
  ws.close()
  "
  ```

  Expected: `"code": 0` response. No errors in the adapter or remote_controller logs.
