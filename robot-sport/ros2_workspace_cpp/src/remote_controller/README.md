# Remote Controller - WebSocket 运动控制桥接器

## 项目概述

Remote Controller 是一个基于 ROS2 的 WebSocket 运动控制桥接器，允许通过 WebSocket 协议远程控制机器人的运动。该项目提供了实时、可靠的机器人运动控制接口，支持多机器人系统和灵活的配置管理。

## 功能特性

- 🌐 **WebSocket 通信**: 基于 WebSocket 协议实现实时双向通信
- 🤖 **ROS2 集成**: 与 ROS2 生态系统无缝集成，发布标准 Twist 消息
- 🔧 **灵活配置**: 支持 JSON 配置文件；环境变量覆盖当前仅针对 ROS 参数
- 📡 **实时控制**: 支持高频率的运动控制命令（推荐 ≤ 50Hz）
- 🛡️ **错误处理**: 完善的输入验证和错误响应机制
- 🎯 **多机器人支持**: 通过动态话题名称支持多机器人系统
- 🧪 **测试工具**: 提供 Python 和 HTML 测试客户端

## 系统架构

```
                    WebSocket 客户端
                           ↓
    ┌─────────────────────────────────────┐
    │        WebSocket 服务器              │
    │     (端口 9099，可配置)               │
    └─────────────────┬───────────────────┘
                      ↓ JSON 消息验证
    ┌─────────────────────────────────────┐
    │       消息验证器                      │
    │   (速度范围、数据类型检查)              │
    └─────────────────┬───────────────────┘
                      ↓ 转换为 Twist 消息
    ┌─────────────────────────────────────┐
    │      ROS2 发布器                     │
    │    (发布到 /{HUB_ID}/cmd_vel)        │
    └─────────────────┬───────────────────┘
                      ↓
            ROS2 机器人控制系统
```

## 快速开始

### 1. 系统要求

- Ubuntu 20.04 或更高版本
- ROS2 Humble 或更高版本
- CMake 3.8+
- C++14 兼容的编译器

### 2. 依赖安装

```bash
# 安装 ROS2 依赖
sudo apt update
sudo apt install ros-humble-geometry-msgs

# 安装 WebSocket 库
sudo apt install libwebsocketpp-dev

# 安装 JSON 库
sudo apt install nlohmann-json3-dev

# 安装 Python WebSocket 客户端（用于测试）
pip3 install websocket-client
```

### 3. 构建项目

```bash
# 创建工作空间（如果还没有）
mkdir -p ~/robot_ws/src
cd ~/robot_ws/src

# 克隆或复制项目文件到此目录
# cp -r /path/to/remote_controller .

# 构建项目
cd ~/robot_ws
colcon build --packages-select remote_controller

# 设置环境
source install/setup.bash
```

### 4. 基本使用

#### 启动节点

```bash
# 使用默认配置启动
ros2 run remote_controller remote_controller_node

# 使用自定义机器人 ID
export HUB_ID="my_robot"
ros2 run remote_controller remote_controller_node
```

#### 发送控制命令

使用 Python 客户端测试：

```bash
cd ~/robot_ws/src/remote_controller/test
python3 test.py
```

或者使用自定义 WebSocket 客户端发送 JSON 消息：

```json
{
    "linear_x": 0.5,    // 线速度 (m/s)
    "angular_z": 0.3    // 角速度 (rad/s)
}
```

#### 监控输出

```bash
# 查看发布的话题
ros2 topic list

# 监控速度命令（替换 YOUR_HUB_ID 为实际值）
ros2 topic echo /YOUR_HUB_ID/cmd_vel
```

## 配置系统

### 配置优先级

配置系统支持多级配置，按以下优先级顺序（从高到低）：

1. **环境变量**（当前仅覆盖 ROS 参数）  
2. **JSON 配置文件**（支持 WebSocket、ROS、Logging 全部配置项）  
3. **默认值**

提示：WebSocket（如 host、port、max_connections）与 Logging（level、enable_websocket_logs）请通过 JSON 配置文件设置；环境变量目前只用于 ROS 相关参数。

### 支持的环境变量

```bash
# 机器人标识符，用于话题命名（仅 ROS）
export HUB_ID="robot_001"

# ROS 话题队列大小（仅 ROS）
export TWIST_TOPIC_QUEUE_SIZE=15

# 自定义配置文件路径（全局）
export REMOTE_CONTROLLER_CONFIG="/path/to/custom_config.json"
```

### 配置文件示例

默认配置文件 `config/remote_controller_config.json`：

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
  }
}
```

## 消息格式

### 输入消息 (WebSocket → 服务器)

```json
{
    "linear_x": 0.5,    // 必需，线速度 (m/s)，范围: [-5.0, 5.0]
    "angular_z": 0.3    // 必需，角速度 (rad/s)，范围: [-3.14, 3.14]
}
```

### 输出消息 (服务器 → WebSocket)

#### 成功响应

```json
{
    "code": 0,
    "msg": "success",
    "data": {
        "velocity": {
            "linear_x": 0.5,
            "angular_z": 0.3
        },
        "target": {
            "topic": "/robot_001/cmd_vel",
            "hub_id": "robot_001"
        }
    },
    "requestId": "req_123456"
}
```

#### 错误响应

```json
{
    "code": 1001,
    "msg": "Required field 'linear_x' is missing",
    "data": {
        "error_details": {
            "code": 1001,
            "message": "Required field 'linear_x' is missing",
            "field": "linear_x",
            "suggestion": "Please include 'linear_x' in your request"
        }
    },
    "requestId": "req_123457"
}
```

### ROS2 输出消息

发布到 `/{HUB_ID}/cmd_vel` 话题的 `geometry_msgs/Twist` 消息：

```yaml
linear:
  x: 0.5  # 从 linear_x 映射
  y: 0.0  # 固定为 0
  z: 0.0  # 固定为 0
angular:
  x: 0.0  # 固定为 0
  y: 0.0  # 固定为 0
  z: 0.3  # 从 angular_z 映射
```

## 测试工具

### Python 测试客户端

```bash
cd test/
python3 test.py
```

**主要功能：**
- 连续发送运动控制命令
- 实时响应验证
- 性能统计（处理时间）
- 错误处理演示

### HTML 测试界面

```bash
cd test/
python3 -m http.server 8080
# 访问 http://localhost:8080/test.html
```

**主要功能：**
- 可视化控制界面
- 实时连接状态
- 手动输入速度值
- 响应日志显示

### 响应格式测试

```bash
cd test/
python3 test_responses.py
```

**测试内容：**
- 各种输入验证场景
- 错误响应格式验证
- 性能基准测试

## 使用场景

### 单机器人控制

```bash
# 启动节点
export HUB_ID="main_robot"
ros2 run remote_controller remote_controller_node

# 在另一个终端监控
ros2 topic echo /main_robot/cmd_vel
```

### 多机器人系统

```bash
# 机器人 1
export HUB_ID="robot_1"
ros2 run remote_controller remote_controller_node --ros-args -p port:=9099

# 机器人 2  
export HUB_ID="robot_2"
ros2 run remote_controller remote_controller_node --ros-args -p port:=9100

# 机器人 3
export HUB_ID="robot_3" 
ros2 run remote_controller remote_controller_node --ros-args -p port:=9101
```

### 生产环境部署

```bash
# 使用生产配置
export REMOTE_CONTROLLER_CONFIG="/opt/robot/config/production_config.json"
export HUB_ID="warehouse_robot_001"
export TWIST_TOPIC_QUEUE_SIZE=20

# 启动节点
ros2 run remote_controller remote_controller_node
```

## 故障排除

### 常见问题

| 问题 | 可能原因 | 解决方案 |
|------|----------|----------|
| WebSocket 连接失败 | 端口被占用 | 检查端口状态：`netstat -tlnp \| grep 9099` |
| ROS2 话题未发布 | HUB_ID 未设置 | 设置环境变量：`export HUB_ID="your_robot"` |
| 配置文件加载失败 | 文件路径错误 | 检查文件存在：`ls -la $REMOTE_CONTROLLER_CONFIG` |
| 编译错误 | 依赖缺失 | 重新安装依赖库 |
| 消息验证失败 | 数值超出范围 | 检查线速度 [-5.0, 5.0]，角速度 [-3.14, 3.14] |

### 调试技巧

#### 检查配置加载

```bash
# 启动时会显示配置信息
ros2 run remote_controller remote_controller_node

# 查看输出示例：
# [INFO] [remote_controller]: [Config] HUB_ID: robot_001
# [INFO] [remote_controller]: [Config] WebSocket Port: 9099
```

#### 监控 WebSocket 连接

```bash
# 检查端口监听状态
sudo netstat -tlnp | grep 9099

# 使用 websocket 客户端测试连接
python3 -c "
import websocket
ws = websocket.WebSocket()
ws.connect('ws://localhost:9099')
print('连接成功')
ws.close()
"
```

#### 验证 ROS2 话题

```bash
# 列出所有话题
ros2 topic list

# 检查话题类型
ros2 topic info /{HUB_ID}/cmd_vel

# 监控消息频率
ros2 topic hz /{HUB_ID}/cmd_vel
```

## 性能优化

### 推荐设置

| 参数 | 推荐值 | 说明 |
|------|--------|------|
| 发送频率 | ≤ 50Hz | 避免过载系统 |
| 队列大小 | 10-20 | 平衡延迟和可靠性 |
| 最大连接数 | 5-10 | 根据硬件能力调整 |

### 性能监控

```bash
# 监控处理延迟
ros2 topic echo /{HUB_ID}/cmd_vel --once

# 查看系统资源使用
htop

# 监控网络流量
iftop
```

## 安全考虑

1. **网络安全**: 在生产环境中使用防火墙限制访问
2. **输入验证**: 所有输入都经过严格验证
3. **连接限制**: 配置最大连接数防止 DoS 攻击
4. **日志审计**: 启用详细日志记录用于安全审计

## 相关文档

- [开发文档](DEVELOPMENT.md) - 详细的开发指南和架构说明
- [API 文档](API.md) - 完整的 WebSocket 与 MQTT API 规范
- [配置文档](CONFIGURATION.md) - 配置系统详细说明


## 贡献

欢迎提交 Issue 和 Pull Request 来改进项目！