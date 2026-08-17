# Robot Adapter 开发者指南

本文档面向需要适配新机器人类型（本体）的开发者，说明如何通过 `robot_adapter_interfaces` 接口实现新的适配器。

---

## 1. 架构概述

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           robot_switch_server (HTTP)                         │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                   AdapterRuntimeManager (生命周期管理)                 │
│  │                     - 进程 fork/execvp 启动/停止                       │
│  │                     - 状态机: Disconnected / Connecting / Connected    │
│  │                               / Disconnecting / Error                  │
│  └───────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
                                       │
                                       │ ROS2 Services
                                       ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                      robot_adapter_interfaces (共享接口)                      │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────────┐   │
│  │    types.hpp     │  │  adapter_client  │  │    SwitchState enum      │   │
│  │  - ErrorCode     │  │  (ROS2 client)   │  │    (状态机定义)           │   │
│  │  - AdapterStatus │  └──────────────────┘  └──────────────────────────┘   │
│  └──────────────────┘                                                       │
└─────────────────────────────────────────────────────────────────────────────┘
                                       │
                                       │ 标准服务接口 (std_srvs/Trigger)
                                       ▼
┌─────────────────────────┐  ┌─────────────────────────┐  ┌──────────────────┐
│     adapter_go2         │  │     adapter_lynx        │  │  adapter_<新本体> │
│  (Unitree Go2 机器狗)    │  │  (DeepRobotics 山猫)    │  │  (你需要实现)     │
└─────────────────────────┘  └─────────────────────────┘  └──────────────────┘
```

除了标准服务接口外，`robot_adapter_interfaces` 还统一维护 `/system_info` 的返回结构。各 adapter 不应再直接把厂商原始 JSON 透传到 HTTP 层，而应先抽取公共字段，再把完整设备快照放入统一的 `details` 字段中。

---

## 2. 核心接口定义

### 2.1 状态枚举 (`types.hpp`)

```cpp
namespace robot_adapter_interfaces {

// 切换状态机 - 每个本体都遵循此状态流转
enum class SwitchState {
    kDisconnected = 0,  // 初始/断开状态
    kConnecting = 1,    // 连接中
    kConnected = 2,     // 已连接，可控制
    kError = 3,         // 错误状态
    kDisconnecting = 4, // 断开中
};

// 错误码 - 标准化错误类型
enum class ErrorCode {
    kNone = 0,
    kUnknownRobot = 1,        // 未知的机器人类型
    kTargetUnavailable = 2,   // 目标不可用
    kBusy = 3,                // 系统忙（正在切换）
    kConnectFailed = 4,       // 连接失败
    kDisconnectFailed = 5,    // 断开失败
    kPreconditionRequired = 6,// 前置条件未满足
};

} // namespace robot_adapter_interfaces
```

### 2.2 标准服务接口

每个适配器必须实现以下 ROS2 服务（使用 `std_srvs/srv/Trigger`）：

| 服务名称 | 功能 | 必填 |
|---------|------|------|
| `/adapter_<type>/connect` | 建立与硬件的连接 | ✅ |
| `/adapter_<type>/disconnect` | 断开硬件连接 | ✅ |
| `/adapter_<type>/safe_stop` | 安全停止运动 | ✅ |
| `/adapter_<type>/health` | 健康检查 | ✅ |
| `/adapter_<type>/system_info` | 获取统一 schema 的系统信息 | ✅ |

> 注：`<type>` 是机器人类型标识符，如 `go2`, `m20pro` 等。

### 2.3 `/system_info` 返回 Schema（必须遵守）

当 adapter 的 `OnSystemInfo()` 能构造系统快照时，`response->message` 必须返回一个 JSON object 字符串，且结构固定为：

```json
{
  "battery": 87,
  "motion": {
    "x": 0.02,
    "y": 0.0,
    "yaw": -0.1
  },
  "motions": [
    {
      "id": "stand_up",
      "service_suffix": "stand_up",
      "description": "Recover to standing posture",
      "display_name": "站立"
    }
  ],
  "details": {
    "vendor_model": "your_robot",
    "battery_pct": 87,
    "velocity": {
      "x": 0.02,
      "y": 0.0,
      "yaw": -0.1
    }
  }
}
```

字段定义：

| 字段 | 类型 | 说明 |
|------|------|------|
| `battery` | `int \| int[] \| null` | 电量百分比。单电池用单个 `int`，多电池用 `int[]`，拿不到时返回 `null` |
| `motion` | `object \| null` | 机器人检测到的当前运动状态，固定为 `{x: float, y: float, yaw: float}`，拿不到时返回 `null` |
| `motions` | `object[]` | 本 adapter 声明的离散动作集合，元素为 `{id, service_suffix, description, display_name}`。未调用 `SetMotions()` 时整个键不出现 |
| `details` | `object` | 设备完整快照，保留厂商/设备原始字段，允许包含比公共 schema 更多的字段 |

约束要求：

- `details` 必须是 object，不能是纯字符串或数组。
- `details` 中应保留 adapter 原始快照里的核心字段，包括电池与运动相关原始字段，即使这些字段已经被抽取到顶层的 `battery` 和 `motion`。
- `battery` 和 `motion` 是对外稳定字段；新增 adapter 时必须优先保证这两个字段语义不变。
- 当 adapter 仍能提供设备快照，但状态为未连接、无效或 stale 时，`response->success` 应返回 `false`，但 `response->message` 仍应返回完整的 `/system_info` schema JSON。
- 只有在 adapter 连快照都无法构造时，`response->message` 才返回纯错误文本。
- `motions[*].id` 必须是非空的 `[A-Za-z0-9_]+`，`service_suffix` 不能为空；违反任一条的动作会被 `SetMotions()` 直接丢弃并打印告警。
- `motions[*].display_name` 是前端直接渲染的中文短名（2-4 字）。留空时 `SetMotions()` 会回填 `id`，所以调用方永远拿不到空串；但请不要依赖这个回落，新增动作时把短名填全。
- `display_name` 必须包含实际可见字符；纯空白或零宽字符（如单个空格、零宽空格）不会被 `.empty()` 判定为空，因此不会触发回落，会作为“非空”值原样传给前端，渲染成一个看不见文字的空按钮。
- `MotionDescriptor` 的 `display_name` 必须保持在最后一个成员。所有调用点都用位置聚合初始化，把它挪到 `description` 之前不会编译失败，只会把英文长句静默塞进短名字段。

> 注：本次发布中 `MotionDescriptor` 新增了 `display_name` 成员，结构体内存布局随之变化。`SystemInfoBuilder::SetMotions(std::vector<MotionDescriptor>)` 是 `librobot_adapter_interfaces.so` 的导出符号，其符号修饰名不编码结构体大小。任何 out-of-tree adapter 在升级本包头文件后，如果没有针对新头文件重新编译，其二进制仍会按旧的（更短的）内存布局构造和传递 `MotionDescriptor`——链接期和动态加载期都不会报错，这是未定义行为。升级本包后，请务必重新编译所有 out-of-tree adapter，不要只替换头文件或只重启进程。

多电池示例：

```json
{
  "battery": [92, 89],
  "motion": {
    "x": 0.0,
    "y": 0.0,
    "yaw": 0.0
  },
  "details": {
    "battery_pack_left": 92,
    "battery_pack_right": 89,
    "velocity": {
      "x": 0.0,
      "y": 0.0,
      "yaw": 0.0
    }
  }
}
```

推荐使用 `robot_adapter_interfaces::SystemInfoBuilder` 生成这份 JSON，避免各 adapter 自行拼接导致字段漂移。

### 2.3.1 `/health` 与 `/system_info` 的状态语义

这两个接口现在表达的是“设备当前是否可用、数据是否新鲜”，不是“ROS service 是否能调通”。

新增或改造 adapter 时，至少要接入以下状态：

- 连接状态：例如 `connected_`
- 最近一次有效设备状态快照
- 最近一次快照时间戳：例如 `last_status_time_`
- 与厂商状态上报频率匹配的 stale timeout
- 单独的 connect readiness timeout，不要复用 query 类 timeout

返回值约定：

- `/health`
  `response->success=true` 仅表示“设备当前健康可用”，典型条件是 `connected && status_valid && !stale`
- `/health`
  `response->message` 应始终是 JSON object 字符串，至少包含 `connected`，并建议包含 `valid`、`fresh`、`stale_ms` 或等价字段，方便上层区分“接口可达但设备不健康”
- `/system_info`
  `response->message` 在大多数情况下都应返回统一 schema JSON；即使 `response->success=false`，也应尽量保留 `details` 快照，供上层排障与展示
- `/system_info`
  顶层 `battery` / `motion` 只应在数据新鲜时设置；当状态 stale 或无效时，应省略这些字段或让其为 `null`，但 `details` 里仍应说明 `connected`、`valid`、`stale`

接入 checklist：

1. `connect` 不要仅以“SDK 初始化成功”判定成功，应等到首帧有效状态到达，或等价的设备 ready 证据出现。
2. `health` 必须基于真实设备状态判断，不能固定返回 `success=true`。
3. `system_info` 必须继续返回统一 schema，并把厂商原始状态放到 `details`。
4. stale timeout 要和状态推送频率匹配，同时要注意不要大于上层调用链允许的 RPC 超时。

### 2.4 适配器职责边界

`robot_adapter_interfaces` 中的 adapter 负责本体直控，不负责导航任务编排。

允许的扩展能力：
- 速度控制，例如 `cmd_vel`
- 启停与安全停机
- 姿态/站立/趴下等本体姿态控制
- 与底层运控直接相关的模式、步态、运动状态切换
- 与本体设备直控相关的灯光、充电、休眠等开关类能力

禁止在 adapter 中暴露的能力：
- 导航目标下发，例如 `nav/goal`
- 导航任务控制与查询，例如 `nav/cancel`、`nav/status`
- 地图定位、重定位、地图位姿查询
- 导航感知状态查询
- 仅用于导航链路的模式或步态入口，例如 `mode/navigation`、`gait/nav_flat`

如果某个模式会使 `cmd_vel`、`safe_stop` 等基础控制链路失效，则该模式不应由 adapter 对外暴露。

---

## 3. 实现新适配器的步骤

### 3.1 创建包结构

```bash
cd src
# 创建新的适配器包
mkdir adapter_<your_robot>
cd adapter_<your_robot>
mkdir -p src include/adapter_<your_robot>
```

**CMakeLists.txt 模板：**

```cmake
cmake_minimum_required(VERSION 3.16)
project(adapter_<your_robot>)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)
# 按需添加其他 ROS2 消息包
# find_package(geometry_msgs REQUIRED)
# find_package(sensor_msgs REQUIRED)

# 如有厂商 SDK，在此引入
# add_subdirectory(vendor_sdk)

add_executable(adapter_<your_robot>_node src/adapter_<your_robot>_node.cpp)
ament_target_dependencies(adapter_<your_robot>_node
  rclcpp
  std_srvs
  robot_adapter_interfaces
)
target_link_libraries(adapter_<your_robot>_node nlohmann_json::nlohmann_json)

# 链接厂商 SDK（如有）
# target_link_libraries(adapter_<your_robot>_node vendor_sdk)

install(TARGETS adapter_<your_robot>_node DESTINATION lib/${PROJECT_NAME})
ament_package()
```

**package.xml 模板：**

```xml
<?xml version="1.0"?>
<package format="3">
  <name>adapter_<your_robot></name>
  <version>0.1.0</version>
  <description>Adapter for <Your Robot></description>
  <maintainer email="dev@example.com">your-team</maintainer>
  <license>Apache-2.0</license>

  <buildtool_depend>ament_cmake</buildtool_depend>
  <depend>rclcpp</depend>
  <depend>std_srvs</depend>
  <depend>nlohmann_json</depend>
  <depend>robot_adapter_interfaces</depend>

  <export>
    <build_type>ament_cmake</build_type>
  </export>
</package>
```

### 3.2 实现适配器节点

适配器节点的核心是一个 ROS2 Node，实现标准服务接口。

**最小实现框架：**

```cpp
#include <chrono>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <rclcpp/rclcpp.hpp>
#include <robot_adapter_interfaces/system_info.hpp>
#include <std_srvs/srv/trigger.hpp>

namespace {

class YourRobotAdapterNode : public rclcpp::Node {
public:
    YourRobotAdapterNode() : Node("adapter_<your_robot>") {
        // 1. 声明参数
        device_ip_ = declare_parameter<std::string>("device_ip", "192.168.1.100");
        timeout_sec_ = declare_parameter<double>("timeout_sec", 10.0);

        // 2. 创建标准服务
        connect_srv_ = create_service<std_srvs::srv::Trigger>(
            "/adapter_<your_robot>/connect",
            [this](auto /*req*/, auto res) { OnConnect(res); });

        disconnect_srv_ = create_service<std_srvs::srv::Trigger>(
            "/adapter_<your_robot>/disconnect",
            [this](auto /*req*/, auto res) { OnDisconnect(res); });

        safe_stop_srv_ = create_service<std_srvs::srv::Trigger>(
            "/adapter_<your_robot>/safe_stop",
            [this](auto /*req*/, auto res) { OnSafeStop(res); });

        health_srv_ = create_service<std_srvs::srv::Trigger>(
            "/adapter_<your_robot>/health",
            [this](auto /*req*/, auto res) { OnHealth(res); });

        system_info_srv_ = create_service<std_srvs::srv::Trigger>(
            "/adapter_<your_robot>/system_info",
            [this](auto /*req*/, auto res) { OnSystemInfo(res); });

        RCLCPP_INFO(get_logger(), "Adapter started for <your_robot>");
    }

private:
    // ========== 核心接口实现 ==========

    void OnConnect(std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (connected_) {
            response->success = true;
            response->message = "Already connected";
            return;
        }

        try {
            // 初始化厂商 SDK，建立连接
            // sdk_->Connect(device_ip_);
            // 等待首帧状态到达，再宣布 connect 成功
            // if (!WaitForFirstStatusFrame(std::chrono::milliseconds(connect_ready_timeout_ms_))) {
            //     response->success = false;
            //     response->message = "Connect failed: no valid status received";
            //     return;
            // }

            connected_ = true;
            response->success = true;
            response->message = "Connected successfully";
        } catch (const std::exception& e) {
            response->success = false;
            response->message = std::string("Connect failed: ") + e.what();
        }
    }

    void OnDisconnect(std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!connected_) {
            response->success = true;
            response->message = "Already disconnected";
            return;
        }

        try {
            // 执行安全停止
            // sdk_->Stop();
            // 断开连接
            // sdk_->Disconnect();

            connected_ = false;
            response->success = true;
            response->message = "Disconnected successfully";
        } catch (const std::exception& e) {
            connected_ = false; // 强制置为断开
            response->success = false;
            response->message = std::string("Disconnect error: ") + e.what();
        }
    }

    void OnSafeStop(std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!connected_) {
            response->success = true;
            response->message = "Not connected, nothing to stop";
            return;
        }

        try {
            // 发送急停/停止命令
            // sdk_->EmergencyStop();

            response->success = true;
            response->message = "Safe stop executed";
        } catch (const std::exception& e) {
            response->success = false;
            response->message = std::string("Safe stop failed: ") + e.what();
        }
    }

    void OnHealth(std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto now = std::chrono::steady_clock::now();
        const int64_t stale_ms = status_valid_
            ? std::chrono::duration_cast<std::chrono::milliseconds>(now - last_status_time_).count()
            : -1;
        const bool fresh = status_valid_ && stale_ms <= status_stale_timeout_ms_;

        nlohmann::json health;
        health["connected"] = connected_;
        health["valid"] = status_valid_;
        health["fresh"] = fresh;
        if (stale_ms >= 0) health["stale_ms"] = stale_ms;

        response->success = connected_ && fresh;
        response->message = health.dump();
    }

    void OnSystemInfo(std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        std::lock_guard<std::mutex> lock(mutex_);

        try {
            const auto now = std::chrono::steady_clock::now();
            const int64_t stale_ms = status_valid_
                ? std::chrono::duration_cast<std::chrono::milliseconds>(now - last_status_time_).count()
                : -1;
            const bool fresh = status_valid_ && stale_ms <= status_stale_timeout_ms_;

            // 获取硬件信息
            // auto info = sdk_->GetSystemInfo();

            nlohmann::json details;
            details["model"] = "YourRobot";
            details["version"] = "1.0.0";
            details["connected"] = connected_;
            details["valid"] = status_valid_;
            details["stale"] = !fresh;
            if (stale_ms >= 0) details["stale_ms"] = stale_ms;
            details["battery_pct"] = 87;
            details["velocity"] = {
                {"x", 0.01},
                {"y", 0.0},
                {"yaw", -0.02},
            };

            // 使用统一 system_info 协议返回数据：
            // - battery: int 或 int[]
            // - motion: {x, y, yaw}
            // - details: 原始完整设备快照，且应保留电池/运动原始字段
            robot_adapter_interfaces::SystemInfoBuilder system_info;
            if (connected_ && fresh) {
                system_info.SetBattery(87);
                system_info.SetMotion(0.01, 0.0, -0.02);
            }
            system_info.SetDetailsJson(details.dump());

            response->success = connected_ && fresh;
            response->message = system_info.Build();
        } catch (const std::exception& e) {
            response->success = false;
            response->message = std::string("Failed to get info: ") + e.what();
        }
    }

    // ========== 成员变量 ==========
    std::string device_ip_;
    double timeout_sec_{10.0};
    int connect_ready_timeout_ms_{1000};
    int status_stale_timeout_ms_{2000};
    bool connected_{false};
    bool status_valid_{false};
    std::chrono::steady_clock::time_point last_status_time_{};
    std::mutex mutex_;

    // 厂商 SDK 实例（如有）
    // std::unique_ptr<VendorSDK> sdk_;

    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr connect_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr disconnect_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr safe_stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr health_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr system_info_srv_;
};

} // namespace

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<YourRobotAdapterNode>());
    rclcpp::shutdown();
    return 0;
}
```

如果设备是多电池，直接传 `std::vector<int>`：

```cpp
robot_adapter_interfaces::SystemInfoBuilder system_info;
system_info.SetBattery(std::vector<int>{92, 89});
system_info.SetMotion(0.0, 0.0, 0.0);
system_info.SetDetailsJson(details.dump());
response->message = system_info.Build();
```

### 3.3 添加运动控制（可选）

如需支持运动控制，可订阅 ROS2 Topic：

```cpp
// 在构造函数中添加
#include <geometry_msgs/msg/twist.hpp>

cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
    "/adapter_<your_robot>/cmd_vel", 10,
    [this](const geometry_msgs::msg::Twist::SharedPtr msg) {
        OnCmdVel(msg);
    });

// 实现回调
void OnCmdVel(const geometry_msgs::msg::Twist::SharedPtr msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) return;

    // 解析速度指令
    float vx = msg->linear.x;
    float vy = msg->linear.y;
    float wz = msg->angular.z;

    // 限速处理
    vx = std::clamp(vx, -max_linear_x_, max_linear_x_);
    // ...

    // 发送给厂商 SDK
    // sdk_->Move(vx, vy, wz);
}
```

### 3.4 添加控制类扩展（可选）

如果机器人厂商 SDK 除了速度控制之外，还需要显式暴露底层控制开关，可以增加扩展 service，但要保持在“本体直控”边界内。

推荐的扩展类型：
- `mode/regular`
- `gait/walk`
- `gait/trot`
- `motion/normal`
- `motion/agile`
- `lights/on`、`lights/off`
- `charge/start`、`charge/stop`
- `sleep/enter`、`sleep/exit`、`sleep/query`

不应新增的扩展类型：
- `mode/navigation`
- `gait/nav_flat`
- 任意 `nav/*`
- 任意定位、地图、感知查询接口

---

## 4. 配置集成

### 4.1 添加到启动配置

编辑 `robot_switch_server/config/server.yaml`：

```yaml
robot_switch_server:
  ros__parameters:
    # 允许启动的适配器类型白名单
    enabled_adapter_types:
      - "go2"
      - "<your_robot>"  # 添加你的机器人类型

    # 各适配器的启动配置
    adapter_config:
      go2:
        auto_stand_on_connect: true
        use_recovery_stand: true
      <your_robot>:
        device_ip: "192.168.1.100"
        timeout_sec: 10.0
```

### 4.2 注册 ament_index

确保 `CMakeLists.txt` 包含正确的安装指令，以便 `robot_switch_server` 通过 `ament_index_cpp` 发现你的适配器：

```cmake
# 确保 package.xml 被正确安装
ament_package()
```

---

## 5. 使用 AdapterClient

### 5.1 客户端初始化

```cpp
#include "robot_adapter_interfaces/adapter_client.hpp"

// 在你的 Node 中创建客户端
auto adapter_client = std::make_unique<robot_adapter_interfaces::AdapterClient>(
    shared_from_this(),           // rclcpp::Node::SharedPtr
    "my_adapter",                 // 适配器名称
    "/adapter_<your_robot>",      // 服务前缀
    std::chrono::milliseconds(500),  // 服务等待超时
    std::chrono::milliseconds(1200)  // 调用超时
);
```

### 5.2 调用服务

```cpp
// 连接
auto result = adapter_client->Connect();
if (!result.ok) {
    RCLCPP_ERROR(get_logger(), "Connect failed: %s", result.message.c_str());
}

// 检查健康状态
auto health = adapter_client->Health();
if (health.reachable && health.ok) {
    RCLCPP_INFO(get_logger(), "Adapter is healthy: %s", health.message.c_str());
} else if (health.reachable) {
    RCLCPP_WARN(get_logger(), "Adapter is reachable but unhealthy: %s", health.message.c_str());
}

// 安全停止
adapter_client->SafeStop();

// 断开连接
adapter_client->Disconnect();
```

### 5.3 AdapterCallResult 结构

```cpp
struct AdapterCallResult {
    bool ok{false};         // 调用是否成功（response->success）
    bool reachable{false};  // 服务是否可达
    std::string message;    // 返回的消息
};
```

对 `/health` 和 `/system_info`，请这样理解：

- `reachable=false`：服务没调通，属于 transport / service discovery 问题
- `reachable=true, ok=false`：服务调通了，但 adapter 判定设备未连接、状态无效或 stale
- `reachable=true, ok=true`：服务调通，且设备状态满足当前可用性要求

其中 `/system_info` 在 `reachable=true` 时，`message` 通常仍应是可解析的统一 schema JSON，即使 `ok=false`。

---

## 6. 参考实现：adapter_go2

### 6.1 目录结构

```
adapter_go2/
├── CMakeLists.txt
├── package.xml
├── unitree_sdk2/          # 本地集成的厂商 SDK
└── src/
    └── adapter_go2_node.cpp
```

### 6.2 关键实现要点

1. **SDK 延迟初始化**：在 `connect` 时才初始化 SDK，避免进程启动就崩溃
2. **异常处理**：所有 SDK 调用都包裹 try-catch
3. **状态保护**：使用 `std::mutex` 保护连接状态
4. **参数可配置**：网络接口、速度限制等都通过 ROS2 参数暴露
5. **线程安全**：ROS2 回调和 SDK 回调使用不同的 mutex

### 6.3 服务响应约定

- `connect` / `disconnect` / `safe_stop`
  `success=true` 表示动作成功或幂等成功；`success=false` 时 `message` 返回错误文本
- `/health`
  `success=true` 表示设备当前健康；`success=false` 表示设备当前不可用、未连接或状态 stale，但 `message` 仍应返回 JSON 状态快照
- `/system_info`
  `success=true` 表示返回的公共字段可直接用于上层；`success=false` 表示快照存在但状态不满足可用性要求，此时 `message` 仍应尽量返回统一 schema JSON
- `message` 如为错误文本，应保持简短且不含换行

---

## 7. 调试与测试

### 7.1 单独测试适配器

```bash
# 1. 构建
source /opt/ros/humble/setup.bash
colcon build --packages-select adapter_<your_robot>

# 2. 运行
source install/setup.bash
ros2 run adapter_<your_robot> adapter_<your_robot>_node

# 3. 测试服务
ros2 service call /adapter_<your_robot>/health std_srvs/srv/Trigger
ros2 service call /adapter_<your_robot>/connect std_srvs/srv/Trigger
ros2 service call /adapter_<your_robot>/system_info std_srvs/srv/Trigger
```

### 7.2 测试运动控制

```bash
# 发布速度指令
ros2 topic pub /adapter_<your_robot>/cmd_vel geometry_msgs/msg/Twist \
    '{linear: {x: 0.5, y: 0.0, z: 0.0}, angular: {x: 0.0, y: 0.0, z: 0.0}}'
```

### 7.3 与 switch_server 集成测试

```bash
# 启动完整系统
ros2 launch robot_switch_server robot_switch_system.launch.py

# HTTP 接口测试
curl http://localhost:8080/status
curl -X POST "http://localhost:8080/start?robot_type=<your_robot>"
```

---

## 8. 常见问题

### Q: 适配器进程启动后立即退出？
- 检查 `ament_target_dependencies` 是否正确声明
- 查看 `ros2 run` 的输出日志
- 确保没有重复的节点名

### Q: switch_server 无法发现我的适配器？
- 确认包名遵循 `adapter_<type>` 格式
- 确认可执行文件名为 `adapter_<type>_node`
- 运行 `colcon build` 后重新 source

### Q: 服务调用超时？
- 检查适配器进程是否正常运行
- 检查服务名是否拼写正确
- 使用 `ros2 service list` 验证服务存在

### Q: 如何处理厂商 SDK 的异步回调？
- 使用 `std::mutex` 保护共享状态
- 考虑使用 `rclcpp::executors::MultiThreadedExecutor`
- 或让 SDK 回调只更新原子变量，主线程轮询

---

## 9. 命名规范

| 项目 | 规范 | 示例 |
|-----|------|------|
| 包名 | `adapter_<type>` | `adapter_go2`, `adapter_lynx` |
| 可执行文件名 | `adapter_<type>_node` | `adapter_go2_node`, `adapter_lynx_node` |
| 服务前缀 | `/adapter_<type>/` | `/adapter_go2/connect` |
| 类型标识符 | 小写+下划线 | `go2`, `lynx`, `unitree_h1` |
| ROS 节点名 | `adapter_<type>` | `adapter_go2`, `adapter_lynx` |

---

## 10. 进阶：扩展接口

如需扩展标准接口（如添加特定功能），建议：

1. **保持向后兼容**：始终实现 5 个标准服务
2. **添加新服务**：使用命名空间避免冲突，如 `/adapter_go2/sport/stand`
3. **添加新 Topic**：如 `/adapter_go2/battery_state`
4. **文档化扩展**：在适配器 README 中说明扩展接口

---

如需更多帮助，请参考：
- `adapter_go2/src/adapter_go2_node.cpp` - 完整参考实现（Unitree Go2）
- `adapter_lynx/src/` - 完整参考实现（DeepRobotics 山猫）
- `robot_adapter_interfaces/include/` - 接口头文件
- `robot_switch_server/src/core/adapter_runtime_manager.cpp` - 生命周期管理

---

## 11. 参考实现：adapter_lynx（DeepRobotics 山猫）

### 11.1 背景与选型原因

山猫（DeepRobotics Lynx / M20 Pro）使用**自定义 UDP 二进制协议**，与 Unitree Go2 使用厂商 SDK 的方式不同，因此需要从零实现底层通信层。选择山猫的原因：

- **四足运动平台**：适用于复杂地形巡检、导航任务
- **开放协议**：官方提供 UDP/TCP 应用层文档，无需闭源 SDK 依赖
- **导航能力（M20 Pro）**：内置定位、避障、单点导航，适合自主巡检场景
- **自主充电**：支持自动回充，可实现长时间无人值守运行

### 11.2 双层架构设计

```
LynxAdapterNode  （ROS2 层）
    ↕  调用
LynxSdkClient    （UDP 通信层）
    ↕  UDP socket
山猫机器人  10.21.31.103:30000
```

**为什么分两层？**

- `LynxSdkClient` 只负责网络通信，不依赖 ROS2，便于单独测试和复用
- `LynxAdapterNode` 只负责 ROS2 接口映射，业务逻辑与协议细节解耦
- 与 go2 的单文件实现相比，山猫协议更复杂（多线程、心跳、状态解析），分层后更易维护

### 11.3 协议说明

山猫使用**固定 16 字节头 + JSON Payload** 的二进制协议：

```
字节 0-3:  同步头  0xEB 0x91 0xEB 0x90
字节 4-5:  JSON 长度（小端 uint16）
字节 6-7:  报文 ID（小端 uint16，自增）
字节 8:    格式标识  0x01 = JSON
字节 9-15: 预留，填 0x00
字节 16+:  JSON Payload
```

JSON 外层统一格式：
```json
{
  "PatrolDevice": {
    "Type": <功能类型>,
    "Command": <指令>,
    "Time": "2026-03-23 12:00:00",
    "Items": { ... }
  }
}
```

### 11.4 目录结构

```
adapter_lynx/
├── CMakeLists.txt
├── package.xml
├── config/
│   └── adapter_lynx.yaml          # 机器人 IP、速度限制等配置
├── include/adapter_lynx/
│   ├── lynx_sdk_client.hpp        # UDP 通信层接口
│   ├── lynx_adapter_node.hpp      # ROS2 节点接口
│   └── lynx_velocity_converter.hpp # SI 速度到 Lynx 轴比例的转换接口
├── src/
│   ├── lynx_sdk_client.cpp        # 协议实现、心跳线程、状态解析
│   ├── lynx_adapter_node.cpp      # ROS2 服务/Topic 实现
│   ├── lynx_velocity_converter.cpp # SI 限速与归一化实现
│   └── main.cpp
└── test/
    ├── test_lynx_velocity_converter.cpp # 速度转换回归测试
    └── test_lynx_sdk_client.cpp       # Lynx UDP 指令与状态确认测试
```

### 11.5 扩展的 ROS2 服务接口

除 5 个标准接口外，lynx 当前还提供以下控制类扩展接口：

| 服务 / Topic | 说明 |
|---|---|
| `/adapter_lynx/cmd_vel` (Topic) | SI 速度入口（linear.x/y: m/s，angular.z: rad/s） |
| `/adapter_lynx/mode/regular` | 切换到常规模式（`Command=21` 轴比例） |
| `/adapter_lynx/mode/navigation` | 切换到导航模式（`Command=25` 绝对速度） |
| `/adapter_lynx/gait/walk` | 兼容别名：按 `standard_flat` 的完整准备和确认流程执行 |
| `/adapter_lynx/gait/trot` | 仅保留服务名；当前协议未定义 4098，调用会明确失败且不下发 |
| `/adapter_lynx/gait/standard_flat` | 标准平地步态（GaitParam=0x1001） |
| `/adapter_lynx/gait/standard_stairs` | 标准楼梯步态（GaitParam=0x1003） |
| `/adapter_lynx/gait/agile_flat` | 敏捷平地步态（GaitParam=0x3002） |
| `/adapter_lynx/gait/agile_stairs` | 敏捷楼梯步态（GaitParam=0x3003） |
| `/adapter_lynx/stand_up` | 站立（MotionParam=1） |
| `/adapter_lynx/soft_stop` | 关节阻尼/软急停（MotionParam=2） |
| `/adapter_lynx/sit_down` | 趴下（MotionParam=4） |
| `/adapter_lynx/rl_control` | 进入 RL 控制（MotionParam=17） |
| `/adapter_lynx/lights/on` | 开前后照明灯 |
| `/adapter_lynx/lights/off` | 关前后照明灯 |
| `/adapter_lynx/charge/start` | 进入自主充电 |
| `/adapter_lynx/charge/stop` | 退出自主充电 |
| `/adapter_lynx/sleep/enter` | 进入休眠模式 |
| `/adapter_lynx/sleep/exit` | 退出休眠模式 |
| `/adapter_lynx/sleep/query` | 查询自动休眠配置 |

说明：
- `adapter_lynx` 已按“本体直控”边界去掉所有导航目标、导航任务、定位和导航感知接口。
- 新的模式、状态和四种步态服务会等待对应 `BasicStatus` 确认后才返回成功，并通过同一控制锁防止与 `cmd_vel` 交错下发。
- 四种步态服务会在内部自动进入 RL 控制。由常规模式请求敏捷步态时，还会自动切到导航模式；辅助模式已支持敏捷步态，因此保留当前模式。
- `mode_regular`、`mode_navigation`、`rl_control`、`gait_standard_flat`、`gait_standard_stairs`、`gait_agile_flat` 和 `gait_agile_stairs` 均通过 HTTP `POST /motion` 对外公开。
- 模式切换或起立后本体会重置为基础步态。调用方重新调用目标步态接口即可，无需单独准备 RL 或导航模式。
- 复合操作在中间阶段失败时保持零速，不自动回滚已完成的本体状态；重试目标步态服务可以幂等续执行。
- 如果新增扩展接口，应遵循本文档第 2.3 节的职责边界。

### 11.6 配置文件

```yaml
# adapter_lynx/config/adapter_lynx.yaml
adapter_lynx:
  ros__parameters:
    robot_ip: "10.21.31.103"
    robot_port: 30000
    # local_port: 0               # 0 = OS-assigned
    heartbeat_interval_sec: 1.0   # 官方建议 >= 1Hz
    recv_timeout_sec: 1.0
    max_linear_x: 2.0             # m/s，Command=21/25 共用安全限制
    max_linear_y: 2.0
    max_angular_z: 2.0            # rad/s
    # Lynx Command=21 的 X/Y/Yaw 是 [-1, 1] 轴比例，不是 SI 速度值
    lynx_full_scale_linear_x_mps: 2.0
    lynx_full_scale_linear_y_mps: 2.0
    lynx_full_scale_angular_z_radps: 2.0
    initial_mode: 0               # 0=常规，1=导航，2=辅助；默认保持旧行为
    initial_motion_state: 1       # 1=站立，17=RL 控制
    cmd_vel_timeout_ms: 500       # 看门狗：超时后自动发零速
    watchdog_check_interval_ms: 100
    query_timeout_ms: 100         # 主动查询类 RPC 超时，不用于 connect readiness
    control_transition_timeout_ms: 1500 # 等待模式/状态/步态确认
    connect_status_timeout_ms: 2000   # 若上层 RPC timeout 更小，需要同步调大调用链超时
    status_stale_timeout_ms: 2000
```

Lynx 高层步态最多会串行等待三次状态确认，因此
`robot_switch_server/config/server.yaml` 的 `call_timeout_ms` 默认设为 `6000`，
应大于 `3 * control_transition_timeout_ms`。

`cmd_vel` 对上层始终使用 `m/s` 和 `rad/s`，上层不需要感知或转换 Lynx 协议值。

- 常规模式（`ControlUsageMode=0`）：适配器先按 `max_*` 限速，再按 `轴比例 = 限速后的 SI 速度 / lynx_full_scale_*` 转为 `Command=21`。例如满量程为 `2.0 m/s` 时，`linear.x=0.7 m/s` 下发为 `X=0.35`。
- 导航模式（`ControlUsageMode=1`）：适配器只按 `max_*` 限速，以 `Command=25` 直接下发 SI 绝对速度。例如 `linear.x=0.7 m/s` 下发仍为 `X=0.7`。
- 辅助模式、非 RL 控制状态或 `BasicStatus` 过期时，适配器拒绝 `cmd_vel`。两种模式都保留死区、物理限速和超时零速看门狗。

### 11.7 测试命令

**直接测试适配器节点（不经过 HTTP）：**

```bash
# 终端 1：启动节点
ros2 run adapter_lynx adapter_lynx_node \
  --ros-args \
  --params-file src/adapter_lynx/config/adapter_lynx.yaml

# 终端 2：基础测试
ros2 service call /adapter_lynx/connect std_srvs/srv/Trigger {}
ros2 service call /adapter_lynx/health std_srvs/srv/Trigger {}
ros2 service call /adapter_lynx/system_info std_srvs/srv/Trigger {}

# 单次步态调用会自动准备导航模式和 RL 控制
ros2 service call /adapter_lynx/gait/agile_flat std_srvs/srv/Trigger {}
# 运动控制（20Hz 持续发送）
ros2 topic pub -r 20 /adapter_lynx/cmd_vel geometry_msgs/msg/Twist \
  "{linear: {x: 0.2, y: 0.0, z: 0.0}, angular: {x: 0.0, y: 0.0, z: 0.0}}"
```

**通过 HTTP 完整链路测试：**

```bash
# 启动 robot_switch_server 后（默认监听 9098）
curl http://localhost:9098/health
curl -X POST "http://localhost:9098/start?adapter_type=lynx" \
  -H "Content-Type: application/json" -d "{}"
curl http://localhost:9098/status
curl http://localhost:9098/system_info
curl -X POST "http://localhost:9098/motion?motion_id=gait_agile_flat"
curl -X POST "http://localhost:9098/motion?motion_id=gait_agile_stairs"
curl -X POST http://localhost:9098/stop \
  -H "Content-Type: application/json" -d "{}"
```

### 11.8 关键实现要点

1. **心跳驱动上报**：机器人只有持续收到心跳（Type=100）才会主动上报状态，`LynxSdkClient` 在独立线程以 1Hz 发送
2. **看门狗安全机制**：`cmd_vel` 超过 500ms 未收到新指令，自动发零速，防止失控
3. **消息 ID 自增**：每次发包 msg_id 递增（`std::atomic<uint16_t>`），机器人用于去重
4. **接收超时非阻塞**：`SO_RCVTIMEO` 设为 1s，接收线程可以定期检查退出标志
5. **线程安全状态缓存**：`latest_status_` 用 `std::mutex` 保护，`ParseReceivedJson` 增量更新
