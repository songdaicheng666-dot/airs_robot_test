# Scout 新接口适配与 Adapter 开发计划

**日期**：2026-08-07  
**状态**：Implemented（已开发，待实机联调）  
**目标平台**：Orsus + Scout 移动底盘  
**目标工作区**：`ros2_workspace_cpp`

## 1. 文档目的

本计划只解决以下两项工作：

1. 将旧 Scout 控制代码从旧 `robot_sport` 接口迁移到当前 `robot_adapter_interfaces` 标准接口。
2. 参考 `adapter_go2`、`adapter_lynx`，在 `ros2_workspace_cpp/src` 下建立正式的 `adapter_scout` ROS 2 C++ package。

这两项工作完成后，本仓库向上游导航模块提供统一的 Scout 底盘速度执行入口：

```text
上游导航模块
    -> /<SN>/cmd_vel
    -> adapter_scout
    -> SocketCAN can0
    -> Scout
```

本计划假设上游导航模块已经负责定位、地图、规划、避障、导航目标以及所需的 odometry/TF。本次不在 `adapter_scout` 中实现这些导航模块能力，只完成上游导航与 Scout 底盘之间的控制桥接。

## 2. 位置与命名结论

新 Scout adapter 放在以下位置是正确的：

```text
/home/zijian/文档/robot-sport/ros2_workspace_cpp/src/adapter_scout
```

原因是当前 C++ 工作区已经采用一个机器人对应一个独立 adapter package 的结构：

```text
ros2_workspace_cpp/src/
├── adapter_go2
├── adapter_lynx
├── adapter_fake
└── adapter_scout       # 本次新增
```

`robot_switch_server` 会根据 `adapter_type` 自动推导 package、可执行文件和服务前缀，因此命名必须固定：

| 项目 | 固定值 |
|---|---|
| Adapter 类型 | `scout` |
| ROS package | `adapter_scout` |
| 可执行文件 | `adapter_scout_node` |
| ROS node | `adapter_scout` |
| 服务前缀 | `/adapter_scout` |
| 配置文件 | `config/adapter_scout.yaml` |

对应推导关系：

```text
adapter_type=scout
    -> package_name=adapter_scout
    -> executable_name=adapter_scout_node
    -> service_prefix=/adapter_scout
```

## 3. 当前旧 Scout 实现

### 3.1 现有代码位置

仓库中存在两份旧 Scout Python 代码：

| 位置 | 用途 |
|---|---|
| `ros2_workspace/src/robot_sport/robot_sport/robot/scout_car.py` | 旧 `robot_sport` 框架中的集成实现 |
| `ros2_workspace/src/scout_car_test_package/scout_car/scout_subscriber.py` | 早期 Scout 独立调试实现 |

旧集成代码通过 `BaseRobotControl` 订阅 `/<HUB_ID>/cmd_vel`，然后在 `ScoutCarControl.control_robot()` 中构造 CAN 帧。

### 3.2 已有控制能力

旧代码已经具备以下底盘控制逻辑：

- 订阅 `geometry_msgs/msg/Twist`
- 使用 `linear.x` 控制前后速度
- 使用 `angular.z` 控制转向
- 使用 500 kbit/s CAN
- 发送 CAN ID `0x421`、数据 `[0x01]` 进入 CAN 模式
- 发送 CAN ID `0x111` 的速度控制帧

当前 `0x111` 数据编码为：

| 字节 | 内容 | 规则 |
|---|---|---|
| 0..1 | 线速度 | `int(linear.x * 1000)`，有符号 16 位，大端序 |
| 2..3 | 角速度 | `int(angular.z * 400)`，有符号 16 位，大端序 |
| 4..7 | 保留 | 全部为 `0x00` |

当前编码范围：

- 线速度 raw：`[-1500, 1500]`，对应配置上限 `1.5 m/s`
- 角速度 raw：`[-523, 523]`，按现有乘数对应配置上限 `1.3075 rad/s`

本次迁移保持现有 CAN ID、字节序、比例和范围，不在接口迁移过程中改变底盘协议。

### 3.3 旧实现与新接口的差距

旧代码没有：

- 标准 `/connect`、`/disconnect`、`/safe_stop`、`/health`、`/system_info` 服务
- `robot_switch_server` 进程生命周期管理
- `cmd_vel` 看门狗
- 统一的 `SystemInfoBuilder` 返回结构
- 独立、可测试的 CAN 传输层和命令编码层
- 断开和退出前的明确零速流程

旧 Python 代码在新 adapter 验收前保留为协议参考，本次不修改、不删除。

## 4. 任务一：旧接口迁移到新接口

### 4.1 新旧接口映射

| 旧实现 | 新实现 |
|---|---|
| `robot_type:=ScoutCar` 动态加载 Python 类 | `adapter_type=scout` 启动独立进程 |
| 节点构造时直接打开 USB CAN | `/adapter_scout/connect` 打开 `can0` |
| `destroy_node()` 停止设备 | `/adapter_scout/disconnect` 零速后关闭 CAN |
| 没有标准停车服务 | `/adapter_scout/safe_stop` |
| 没有健康接口 | `/adapter_scout/health` |
| 没有统一系统信息 | `/adapter_scout/system_info` |
| 自行读取 `HUB_ID` | 使用 `AdapterNodeBase::GetCmdVelTopic()` |
| Python `gs_usb` 直接访问 USB | C++ raw SocketCAN 访问 `can0` |

### 4.2 标准服务

`ScoutAdapterNode` 继承 `robot_adapter_interfaces::AdapterNodeBase`，实现以下五个接口：

#### `/adapter_scout/connect`

- 重复连接时幂等返回成功。
- 检查配置的 CAN 接口存在且为 UP。
- 打开并绑定 raw SocketCAN socket。
- 发送 `0x421 [0x01]` 初始化帧。
- 紧接着发送一帧 `0x111` 零速度，避免沿用底盘侧残留命令。
- 只有接口绑定、初始化帧和首帧零速度全部发送成功后才标记 `connected=true`。
- 失败时关闭已创建的 socket，并返回明确错误信息。

#### `/adapter_scout/disconnect`

- 未连接时幂等返回成功。
- 先禁止继续接受速度控制。
- 发送 `0x111` 零速度帧。
- 清理命令状态并关闭 SocketCAN。
- 零速发送失败时仍完成资源关闭，但服务返回失败并说明原因。

#### `/adapter_scout/safe_stop`

- 已连接时立即发送 `0x111` 零速度帧。
- 发送成功才返回 `success=true`。
- 未连接时作为无操作幂等成功，并明确返回当前未连接。

#### `/adapter_scout/health`

- 返回 JSON object 字符串。
- 至少包含 `connected`、`transport_ready`、`can_interface` 和 `last_error`。
- `success=true` 表示 adapter 已连接、SocketCAN 可用且没有当前传输错误。

#### `/adapter_scout/system_info`

- 使用 `robot_adapter_interfaces::SystemInfoBuilder`。
- `details` 至少包含 `adapter=scout`、`transport=socketcan`、接口名、连接状态和最后错误。
- 本次没有可靠电量和实际运动反馈时，顶层 `battery`、`motion` 保持 `null`。
- 不声明未确认的离散动作，不调用 `SetMotions()`。

### 4.3 `cmd_vel` 接口

新 adapter 在 `RegisterExtensions()` 中订阅：

```cpp
GetCmdVelTopic()
```

正常部署时主题解析为：

```text
/<SN>/cmd_vel
```

输入处理顺序：

1. 未连接时忽略命令并限频告警。
2. 拒绝 `NaN` 和正负无穷。
3. 读取 `linear.x` 和 `angular.z`。
4. 将输入限制在配置的安全范围内。
5. 使用 `ScoutCommandCodec` 生成 `0x111` 帧。
6. 通过 `ScoutCanClient` 发送。
7. 只有发送成功后才更新最近有效命令时间。

Scout 不支持的 Twist 分量不进入控制帧：

- `linear.y`
- `linear.z`
- `angular.x`
- `angular.y`

这些分量明显非零时只做限频告警，不改变既有 `linear.x + angular.z` 控制语义。

### 4.4 看门狗和零速

新增默认 500 ms 的 `cmd_vel` 看门狗：

- 收到并成功发送非零命令后启动计时。
- 超过 `cmd_vel_timeout_ms` 未收到新的有效命令时发送一次零速度帧。
- 发送零速度后清除计时状态，避免定时器持续重复刷帧和日志。
- 无效输入和发送失败不能刷新看门狗时间。

以下路径都必须发送明确的零速度帧：

- 收到全零 `cmd_vel`
- 看门狗超时
- `/safe_stop`
- `/disconnect`
- 节点正常退出

## 5. 任务二：建立 `adapter_scout` Package

### 5.1 目录结构

计划新增：

```text
ros2_workspace_cpp/src/adapter_scout/
├── CMakeLists.txt
├── package.xml
├── config/
│   └── adapter_scout.yaml
├── include/adapter_scout/
│   ├── scout_adapter_node.hpp
│   ├── scout_can_client.hpp
│   └── scout_command_codec.hpp
├── src/
│   ├── main.cpp
│   ├── scout_adapter_node.cpp
│   ├── scout_can_client.cpp
│   └── scout_command_codec.cpp
└── test/
    ├── test_scout_command_codec.cpp
    └── test_scout_can_client.cpp
```

### 5.2 模块职责

`ScoutAdapterNode`：

- ROS 2 node 和新 adapter 标准接口
- `cmd_vel` 订阅
- 看门狗
- 连接状态与服务响应

`ScoutCanClient`：

- 使用 `PF_CAN/SOCK_RAW/CAN_RAW`
- 解析接口索引并绑定 `can0`
- 发送标准 11-bit CAN 帧
- 管理 socket 生命周期
- 不直接访问 USB，不调用 Python `gs_usb`

`ScoutCommandCodec`：

- 速度限值后的 raw 数值转换
- 有符号 16 位大端编码
- 构造 `0x111` 的 8 字节数据
- 不访问 ROS 和硬件，便于单元测试

`main.cpp`：

- `rclcpp::init()`
- 创建 `ScoutAdapterNode`
- 调用 `node->Init()` 注册扩展 topic/timer
- `rclcpp::spin()`
- 正常退出时完成资源清理

### 5.3 配置文件

建议首版配置：

```yaml
adapter_scout:
    ros__parameters:
        can_interface: "can0"
        max_linear_x_mps: 1.5
        max_angular_z_radps: 1.3075
        cmd_vel_timeout_ms: 500
        watchdog_check_interval_ms: 100
```

`can0` 在 adapter 启动前由 Orsus 部署环境配置为 500 kbit/s：

```bash
ip link set can0 up type can bitrate 500000
```

`adapter_scout` 只使用已经配置好的接口，不在 ROS 回调中执行 `ip`、`sudo` 或 `modprobe`。

### 5.4 构建约定

- 使用 C++17 和 `ament_cmake`。
- 依赖 `rclcpp`、`std_srvs`、`geometry_msgs`、`robot_adapter_interfaces`、`nlohmann_json`。
- 使用 Linux SocketCAN 系统头文件，不引入 Python `gs_usb`。
- 遵循工作区现有 `-Wall -Wextra -Wpedantic` 和可选 `ROBOT_SPORT_WERROR`。
- 安装可执行文件到 `lib/adapter_scout`。
- 安装配置文件到 `share/adapter_scout/config`。
- 使用 `ament_cmake_gtest` 测试 codec 和 CAN client。

## 6. 预计代码改动范围

实施阶段只允许以下范围：

```text
新增：ros2_workspace_cpp/src/adapter_scout/**
修改：ros2_workspace_cpp/src/robot_switch_server/config/server.yaml
修改：changelog.txt
按需修改：与 adapter_scout 直接相关的开发/启动文档
```

`server.yaml` 只需要把 `scout` 加入白名单：

```yaml
enabled_adapter_types:
    - "go2"
    - "lynx"
    - "scout"
```

本次明确不修改：

- `adapter_go2`
- `adapter_lynx`
- `robot_adapter_interfaces` 公共契约
- `remote_controller`
- 上游导航模块
- 旧 Scout Python 实现
- 与 Scout 接入无关的 Debian、SDK 和配置文件

如果开发时发现必须修改上述范围，先更新并评审本计划，不直接扩大修改。

## 7. 实施顺序

### 步骤 1：创建 package 骨架

- 创建 `adapter_scout` 目录。
- 添加 `package.xml`、`CMakeLists.txt`、config、include、src 和 test。
- 确认可执行文件安装名为 `adapter_scout_node`。

### 步骤 2：实现并测试 CAN 编码

- 先实现 `ScoutCommandCodec`。
- 固定 `0x111` 帧格式、范围、字节序和零帧。
- 单元测试通过后再连接 ROS 和硬件。

### 步骤 3：实现 SocketCAN client

- 打开并绑定配置接口。
- 实现初始化帧和控制帧发送。
- 实现错误返回和 RAII 资源清理。
- 使用假传输或 `vcan0` 完成无实车测试。

### 步骤 4：实现 adapter node

- 继承 `AdapterNodeBase`。
- 实现五个标准服务。
- 注册 `cmd_vel` 和看门狗。
- 使用 `SystemInfoBuilder` 返回统一结构。

### 步骤 5：接入 switch server

- 将 `scout` 加入 `enabled_adapter_types`。
- 验证 package、可执行文件和服务前缀能被自动解析。

### 步骤 6：联调上游导航

- 确认上游向 `/<SN>/cmd_vel` 发布 `Twist`。
- 低速验证前进、后退、左转、右转和停车。
- 验证导航停止发布后，Scout 在 500 ms 看门狗超时后停车。

## 8. 测试计划

### 8.1 Codec 单元测试

- 零速度：`00 00 00 00 00 00 00 00`
- `1.0 m/s, 0.5 rad/s`：`03 E8 00 C8 00 00 00 00`
- `-1.0 m/s, -0.5 rad/s`：`FC 18 FF 38 00 00 00 00`
- 正负最大边界
- 超限输入的限幅结果
- `NaN`、正负无穷拒绝
- CAN ID 固定为 `0x111`

### 8.2 CAN client 测试

- 接口不存在时打开失败并返回错误
- `vcan0` 绑定和发帧成功
- `0x421 [0x01]` 初始化帧正确
- socket 关闭后拒绝发送
- 重复打开和关闭保持幂等
- 析构后没有句柄泄漏

### 8.3 Adapter 接口测试

- 五个标准服务都能发现和调用
- 未连接时不发送运动命令
- connect 成功后允许 `cmd_vel`
- safe stop 发送零帧
- disconnect 先零速再关闭
- 看门狗超时发送零帧
- `health` 返回 JSON object
- `system_info` 符合统一 schema

### 8.4 构建和系统冒烟测试

软件阶段验证命令：

```bash
source /opt/ros/humble/setup.bash
colcon build --packages-select robot_adapter_interfaces adapter_scout robot_switch_server
colcon test --packages-select adapter_scout
colcon test-result --verbose
```

启动链验证：

```text
POST /start?adapter_type=scout
    -> adapter_scout_node
    -> /adapter_scout/connect
    -> /<SN>/cmd_vel
    -> CAN 0x111
```

实车测试必须在架空轮组或清空区域内，从低速开始，并准备物理急停。

## 9. 代码开发规范

- C++17，遵循现有 adapter 的命名和目录风格。
- 文件名使用 `snake_case`，类型使用 `PascalCase`。
- CAN 协议转换集中在 codec，不在 ROS callback 中散落字节拼装。
- socket 使用 RAII，禁止裸资源泄漏。
- 不使用 detached thread。
- 共享连接状态和发送操作必须有明确互斥保护。
- 高频 `cmd_vel` 不逐帧打印 info 日志；错误和告警需要限频。
- 参数名称携带单位，例如 `_mps`、`_radps`、`_ms`。
- 异常和失败必须返回明确错误，不固定返回成功。
- 不复制或修改其他 adapter 的厂商实现，只参考它们的结构和接口模式。
- 每次代码变化都在根目录 `changelog.txt` 记录目的和位置。
- 不进行与本计划无关的格式化、重构或文件清理。

## 10. 完成标准

以下条件全部满足后，这两项工作才算完成：

- `adapter_scout` 位于 `ros2_workspace_cpp/src/adapter_scout`。
- package 名、可执行文件名、node 名和服务前缀符合约定。
- `robot_switch_server` 可以通过 `adapter_type=scout` 启停它。
- 五个标准服务按约定工作。
- `/<SN>/cmd_vel` 的 `linear.x`、`angular.z` 能转换为正确的 `0x111` CAN 帧。
- 新实现只通过 SocketCAN `can0` 通信。
- safe stop、disconnect、看门狗和正常退出都能发送零速度。
- 单元测试、接口测试、构建和低速实车测试通过。
- 旧 Scout Python 节点不会与 `adapter_scout_node` 同时运行。
- 上游导航模块可以通过统一 `cmd_vel` 接口驱动 Scout 底盘。

完成上述内容即完成本次限定范围内的接口迁移和 Scout adapter 建设。导航模块内部能力不属于本计划的开发范围。
