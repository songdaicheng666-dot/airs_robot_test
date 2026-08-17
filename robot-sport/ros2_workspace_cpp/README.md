# Robot Sport ROS2 运动控制系统

一个支持多机器人类型（Unitree Go2、M20 Pro 等）的运动控制中间件系统，提供统一的 HTTP 控制接口和 MQTT 遥测上报。

## 系统架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         HTTP 客户端 / 上位机                                  │
│                    (调用 REST API 控制机器人)                                 │
└─────────────────────────────────────────────────────────────────────────────┘
                                       │
                                       ▼ HTTP (默认 8080 端口)
┌─────────────────────────────────────────────────────────────────────────────┐
│                      robot_switch_server (HTTP/MQTT)                         │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                   AdapterRuntimeManager (生命周期管理)                 │
│  │                     - 进程 fork/execvp 启动/停止                       │
│  │                     - 状态机: Disconnected → Connecting → Connected    │
│  │                     - 同时只运行一个适配器                             │
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
                                       │ 标准服务接口 (std_srvs/srv/Trigger)
                                       ▼
┌─────────────────────────┐  ┌─────────────────────────┐  ┌──────────────────┐
│     adapter_go2         │  │    adapter_m20pro       │  │  adapter_<新本体> │
│  (Unitree Go2 机器狗)    │  │  (Future: M20 Pro)       │  │  (可扩展)        │
│  - 运动控制              │  │  - 占位适配器             │  │                  │
│  - 状态监听              │  │                         │  │                  │
└─────────────────────────┘  └─────────────────────────┘  └──────────────────┘
```

## 项目结构

```
ros2_workspace_cpp/
├── src/
│   ├── robot_adapter_interfaces/    # 共享接口定义
│   │   ├── include/
│   │   │   ├── types.hpp            # SwitchState, ErrorCode, AdapterStatus
│   │   │   └── adapter_client.hpp   # C++ 客户端封装
│   │   └── src/
│   │       ├── types.cpp
│   │       └── adapter_client.cpp
│   │
│   ├── robot_switch_server/         # 中央控制服务器
│   │   ├── include/
│   │   │   ├── core/
│   │   │   │   └── adapter_runtime_manager.hpp
│   │   │   ├── http/
│   │   │   │   └── json_response_builder.hpp
│   │   │   ├── infra/
│   │   │   │   ├── http_server_runner.hpp
│   │   │   │   └── mqtt_publisher.hpp
│   │   │   ├── telemetry/
│   │   │   │   └── motion_telemetry_reporter.hpp
│   │   │   └── utils/
│   │   │       ├── json_utils.hpp
│   │   │       └── telemetry_json_builder.hpp
│   │   ├── src/
│   │   ├── config/
│   │   │   └── server.yaml          # 服务器配置文件
│   │   └── launch/
│   │       └── robot_switch_system.launch.py
│   │
│   ├── adapter_go2/                 # Unitree Go2 适配器
│   │   ├── src/
│   │   │   └── adapter_go2_node.cpp
│   │   └── unitree_sdk2/            # 本地集成的厂商 SDK
│   │
│   └── adapter_m20pro/              # M20 Pro 适配器（占位）
│       └── src/
│           └── adapter_m20pro_node.cpp
│
├── CLAUDE.md                        # 开发指南
└── README.md                        # 本文档
```

## 依赖安装

### 基础依赖

```bash
# ROS2 Humble (Ubuntu 22.04)
sudo apt update
sudo apt install -y ros-humble-desktop ros-humble-ament-cmake ros-humble-rclcpp ros-humble-std-srvs ros-humble-geometry-msgs

# 可选：HTTP 服务器支持
# cpp-httplib 是 header-only，已包含在项目中

# 可选：MQTT 遥测支持
sudo apt-get install libpaho-mqtt-dev libpaho-mqttpp-dev
```

### nlohmann-json

```bash
sudo apt install nlohmann-json3-dev
```

## 构建项目

```bash
cd ros2_workspace_cpp

# 加载 ROS2 环境
source /opt/ros/humble/setup.bash

# 完整构建
colcon build

# 或仅构建特定包
colcon build --packages-select robot_adapter_interfaces robot_switch_server adapter_go2

# 加载构建结果
source install/setup.bash
```

## 配置说明

### 服务器配置 (`src/robot_switch_server/config/server.yaml`)

```yaml
robot_switch_server:
  ros__parameters:
    # HTTP 监听地址
    http_listen_address: "0.0.0.0:9098"

    # ROS2 服务调用超时配置 (毫秒)
    service_wait_ms: 500        # 等待服务可用超时
    call_timeout_ms: 6000       # 覆盖 Lynx 模式/RL/步态串行确认
    adapter_connect_timeout_ms: 8000  # 适配器连接超时

    # 允许的机器人类型白名单
    enabled_adapter_types:
      - "go2"
      - "lynx"

    # MQTT 遥测配置
    mqtt:
      enabled: true
      broker: "tcp://127.0.0.1:1882"
      region: "cn-sz"
      tenant_id: "default"
      state_interval_ms: 1000     # 状态上报间隔
      # 设备信息文件路径（用于读取 SN）
      device_info_path: "/workspace/.info/device_info.json"
```

### Go2 适配器配置

适配器支持从 YAML 文件加载配置，配置文件路径：
`<package_share>/config/adapter_go2.yaml`

```yaml
adapter_go2:
  ros__parameters:
    network_interface: "eth0"           # 网络接口
    sdk_timeout_sec: 10.0               # SDK 超时
    auto_stand_on_connect: true         # 连接后自动站立
    use_recovery_stand: true            # 使用恢复站立
    stand_down_on_disconnect: false     # 断开时趴下
    report_interval_sec: 3              # 状态上报间隔
    report_duration_sec: 30             # 状态上报持续时间
    max_linear_x: 1.5                   # 最大前进速度 (m/s)
    max_linear_y: 1.0                   # 最大侧移速度 (m/s)
    max_angular_z: 2.0                  # 最大旋转速度 (rad/s)
```

## 使用指南

### 启动系统

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash

# 使用默认配置启动
ros2 launch robot_switch_server robot_switch_system.launch.py

# 或使用自定义配置
ros2 launch robot_switch_server robot_switch_system.launch.py \
    config_file:=/path/to/your/server.yaml
```

### HTTP API 接口

#### 1. 健康检查

```bash
curl http://localhost:8080/healthz
```

响应：
```json
{"status":"ok"}
```

#### 2. 获取系统状态

```bash
curl http://localhost:8080/status
```

响应：
```json
{
  "active_robot": "go2",
  "adapters": {
    "go2": {
      "available": true,
      "detail": "Adapter is running and connected",
      "reachable": true,
      "registered": true
    }
  },
  "busy": false,
  "last_code": 0,
  "last_detail": "",
  "last_message": "",
  "state": "connected"
}
```

状态说明：
- `state`: `disconnected` | `connecting` | `connected` | `error`
- `busy`: 是否正在进行切换操作
- `active_robot`: 当前激活的机器人类型

#### 3. 启动机器人

```bash
curl -X POST "http://localhost:8080/start?robot_type=go2"
```

响应：
```json
{
  "success": true,
  "message": "Robot go2 started successfully",
  "robot_type": "go2"
}
```

#### 4. 停止当前机器人

```bash
curl -X POST "http://localhost:8080/stop"
```

#### 5. 获取系统信息

```bash
curl http://localhost:8080/system_info
```

Go2 响应示例：
```json
{
  "connected": true,
  "has_low_state": true,
  "has_sport_state": true,
  "low": {
    "battery_cycle": 123,
    "battery_current": 0.5,
    "battery_soc": 85,
    "power_a": 2.1,
    "power_v": 28.5
  },
  "network_interface": "eth0",
  "service_count": 5,
  "service_list_ret": 0,
  "services": [...],
  "sport": {
    "error_code": 0,
    "gait_type": 1,
    "mode": 1,
    "velocity": [0.5, 0.0, 0.0],
    "yaw_speed": 0.1
  }
}
```

### ROS2 命令行工具

#### 单独测试适配器

```bash
# 启动 Go2 适配器
ros2 run adapter_go2 adapter_go2_node

# 测试服务
ros2 service call /adapter_go2/health std_srvs/srv/Trigger
ros2 service call /adapter_go2/connect std_srvs/srv/Trigger
ros2 service call /adapter_go2/system_info std_srvs/srv/Trigger

# 断开连接
ros2 service call /adapter_go2/disconnect std_srvs/srv/Trigger
```

#### 运动控制

```bash
# 发布速度指令 (线速度 x=0.5m/s, 角速度 z=0.1rad/s)
ros2 topic pub /adapter_go2/cmd_vel geometry_msgs/msg/Twist \
    '{linear: {x: 0.5, y: 0.0, z: 0.0}, angular: {x: 0.0, y: 0.0, z: 0.1}}'

# 停止运动
ros2 service call /adapter_go2/stop std_srvs/srv/Trigger

# 切换站立/趴下
ros2 service call /adapter_go2/stand std_srvs/srv/Trigger
ros2 service call /adapter_go2/damp std_srvs/srv/Trigger

# 平衡站立模式
ros2 service call /adapter_go2/balance_stand std_srvs/srv/Trigger
```

#### 查看话题

```bash
# 列出所有话题
ros2 topic list

# 查看适配器状态
ros2 topic echo /robot_status
```

## 状态机

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          SwitchState 状态机                              │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   ┌─────────────┐   /start (launch)   ┌─────────────┐                  │
│   │             │ ──────────────────▶ │             │   health+connect  │
│   │ Disconnected│                     │  Connecting │ ────────────────▶ │
│   │   (初始)    │ ◀────────────────── │             │ ◀───────────────  │
│   │             │  launch/connect 失败 │             │    超时/失败      │
│   └─────────────┘                     └─────────────┘                  │
│          ▲                                    │                        │
│          │                                    │ success                │
│          │                                    ▼                        │
│          │                           ┌─────────────┐     进程崩溃        │
│   /stop  │                           │             │ ────────────────▶ │
│  (任意)  │                           │  Connected  │                   │
│          │                           │  (运行中)   │ ◀──────────────── │
│          │                           │             │      /stop        │
│          │                           └─────────────┘                   │
│          │                                    │                        │
│          │                                    │ 进程意外退出            │
│          │                                    ▼                        │
│          │                           ┌─────────────┐     /stop         │
│          └────────────────────────── │    Error    │ ────────────────▶ │
│                                      │  (异常状态) │                   │
│                                      └─────────────┘                   │
│                                                                         │
│  状态说明:                                                              │
│  • Disconnected: 初始状态，无适配器进程运行                              │
│  • Connecting:  进程已启动，等待服务就绪和连接                           │
│  • Connected:   适配器已连接，可正常控制                                 │
│  • Error:       进程意外退出，需要 /stop 重置                            │
└─────────────────────────────────────────────────────────────────────────┘
```

## 扩展开发

### 添加新的机器人类型

1. **创建新的适配器包**

```bash
cd src
mkdir adapter_<your_robot>
cd adapter_<your_robot>
mkdir -p src include/adapter_<your_robot> config
```

2. **实现适配器节点**

参考 `adapter_go2/src/adapter_go2_node.cpp` 实现 5 个标准服务：
- `/adapter_<type>/connect` - 建立连接
- `/adapter_<type>/disconnect` - 断开连接
- `/adapter_<type>/safe_stop` - 安全停止
- `/adapter_<type>/health` - 健康检查
- `/adapter_<type>/system_info` - 系统信息

3. **注册到服务器配置**

编辑 `robot_switch_server/config/server.yaml`：

```yaml
enabled_adapter_types:
  - "go2"
  - "<your_robot>"
```

详细开发指南请参考：
- `src/robot_adapter_interfaces/ADAPTER_DEVELOPER_GUIDE.md`
- `CLAUDE.md`

## MQTT 遥测

当 `mqtt.enabled=true` 时，系统会定期上报遥测数据。

### 设备信息文件格式

`/workspace/.info/device_info.json`：
```json
{
  "SN": "R2D2-12345"
}
```

### MQTT 主题

- `v1/{region}/{tenant_id}/{device_id}/state` - 状态上报
- `v1/{region}/{tenant_id}/{device_id}/events` - 事件上报

## 故障排查

### 适配器进程启动后立即退出

- 检查 `ament_target_dependencies` 是否正确声明
- 查看 `ros2 run` 的输出日志
- 确保没有重复的节点名

### switch_server 无法发现适配器

- 确认包名遵循 `adapter_<type>` 格式
- 确认可执行文件名为 `adapter_<type>_node`
- 运行 `colcon build` 后重新 source

### 服务调用超时

- 检查适配器进程是否正常运行
- 检查服务名是否拼写正确
- 使用 `ros2 service list` 验证服务存在

### Go2 连接失败

- 检查网络接口配置（默认 eth0）
- 确保与 Go2 在同一网段
- 使用 `ping 192.168.123.161` 测试连通性

## 安全注意事项

1. **急停机制**: 系统提供 `/safe_stop` 和 `/stop` 两个停止接口
2. **速度限制**: 适配器内置速度限制保护
3. **独占控制**: 同时只能有一个适配器处于 connected 状态
4. **自动停止**: 断开连接时自动发送停止命令
