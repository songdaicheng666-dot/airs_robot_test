# Lynx 速度转换链路与限制参数

本文按当前代码与配置整理 Lynx 连续速度控制链路，范围是
`WebSocket/MQTT -> ROS 2 cmd_vel -> adapter_lynx -> Lynx Command=21`。
`robot_switch_server` 的 `/motion` 离散动作接口不在本文讨论范围内。

核对基线：2026-07-21，分支 `fix/adapter-lynx-velocity-conversion` 的当前工作树。

## 1. 结论

1. WebSocket、MQTT 和 ROS `cmd_vel` 对外都使用 SI 单位：线速度是 `m/s`，角速度是 `rad/s`。
2. `remote_controller` 不做速度倍率转换。校验通过后，它把输入值原样写入
   `geometry_msgs/msg/Twist`。
3. SI 速度到 Lynx 轴比例的转换只发生在 `adapter_lynx`：先按 `max_*` 限幅，再除以
   `lynx_full_scale_*`，得到 `[-1, 1]` 的 `X/Y/Yaw`。
4. `LynxSdkClient` 不再转换单位，只校验轴比例、保留 4 位小数，并通过 UDP 发送
   `Type=2, Command=21`。
5. 这是一条开环指令链。Lynx 上报的 `MotionStatus.LinearX/LinearY/OmegaZ` 只用于状态展示，
   当前代码不会根据实测速度做闭环修正或二次限速。

核心公式如下：

```text
limited_i = clamp(input_si_i, -max_i, max_i)
ratio_i   = clamp(limited_i / full_scale_i, -1.0, 1.0)
```

其中 `i` 分别是 `linear_x`、`linear_y` 和 `angular_z`。

## 2. 完整链路

```text
WebSocket ws://<host>:9099                    MQTT downlink
JSON: linear_x/linear_y/angular_z (SI)        同一份 JSON (SI)
             \                                  /
              +------ handleVelocityMessage ---+
                              |
                              v
                  MessageValidator
                  非法值或越界：拒绝整条命令
                              |
                              v
                  VelocityProcessor
                  数值不缩放，直接构造 Twist
                              |
                              v
                       /{SN}/cmd_vel
                              |
                 +------------+------------+
                 |                         |
          adapter_lynx 已连接          未连接
                 |                         |
                 v                         +--> 丢弃
       只取 linear.x/linear.y/angular.z
       每个轴应用 1e-4 SI 死区
                 |
                 v
       LynxVelocityConverter
       SI 限幅 -> 除以 full-scale -> [-1, 1]
                 |
                 v
       LynxSdkClient::SendMotionCmd()
       有限值/范围检查 -> 保留 4 位小数
                 |
                 v
       UDP 10.21.31.103:30000
       Type=2, Command=21, Items.X/Y/Yaw
                 |
                 v
             Lynx 本体控制器
```

还有一条不经过 `remote_controller` 的入口：其他 ROS 2 节点可以直接发布
`geometry_msgs/msg/Twist` 到 `/{SN}/cmd_vel`。这条路径会绕过 WebSocket/MQTT 的范围校验，
但仍会经过 `adapter_lynx` 的死区、SI 限幅和归一化。

### 2.1 控制面不承载逐帧速度

集成启动文件 `robot_switch_system.launch.py` 同时启动：

- `robot_switch_server_node`：HTTP 控制面；
- `remote_controller_node`：WebSocket/MQTT 到 `cmd_vel` 的数据面。

调用 `POST /start?adapter_type=lynx` 后，`robot_switch_server` 才会拉起并连接
`adapter_lynx_node`。之后每一帧速度不会经过 HTTP server，而是直接走 ROS topic。

## 3. 分层行为

### 3.1 WebSocket/MQTT 输入校验

WebSocket 与 MQTT 最终共用 `VelocityProcessor::processVelocityCommand()`。

- `linear_x`、`angular_z` 必填；
- `linear_y`、`linear_z`、`angular_x`、`angular_y` 可选，缺省为 `0.0`；
- 所有已提供字段必须是有限 JSON 数字；
- 越界时拒绝整条命令，不会裁剪，也不会发布 `Twist`；
- 当前只向客户端返回第一条校验错误。

实时代码中的入口范围如下：

| 字段 | 入口允许范围 | 单位 | Lynx 是否使用 |
| --- | ---: | --- | --- |
| `linear_x` | `[-5.0, 5.0]` | `m/s` | 是 |
| `linear_y` | `[-3.0, 3.0]` | `m/s` | 是 |
| `linear_z` | `[-3.0, 3.0]` | `m/s` | 否 |
| `angular_x` | `[-3.0, 3.0]` | `rad/s` | 否 |
| `angular_y` | `[-3.0, 3.0]` | `rad/s` | 否 |
| `angular_z` | `[-3.14, 3.14]` | `rad/s` | 是，映射为 `Yaw` |

这些范围目前硬编码在 `remote_controller`，没有接入 JSON 配置文件。

注意：入口范围比 Lynx adapter 的安全范围宽。例如 `linear_x=5.0` 会通过入口校验，
客户端也会收到“已发布成功”的响应，但 adapter 会在后面将它裁剪到 `max_linear_x`。
因此 WebSocket/MQTT 成功响应不是 Lynx 最终轴指令的回执。

### 3.2 ROS topic 和 SN

`remote_controller` 发布到 `/{SN}/cmd_vel`，`adapter_lynx` 也从同一个设备信息文件读取
`SN` 并订阅该 topic。两边都依赖：

```text
/workspace/.info/device_info.json
```

文件缺失或 `SN` 无效时，两边的 fallback 不一致：

| 节点 | fallback topic |
| --- | --- |
| `remote_controller` | `/DEFAULT_HUB_ID/cmd_vel` |
| `adapter_lynx` | `/adapter_lynx/cmd_vel` |

因此缺少设备 SN 时，集成链路不会自动接通。此时应修复设备信息，而不是通过调大速度参数解决。

ROS publisher 队列深度由 `ros.twist_topic_queue_size` 控制，当前配置为 `10`；
Lynx subscription 队列深度硬编码为 `10`。这两个值只影响排队，不改变速度数值。

### 3.3 Lynx adapter 预处理

`LynxAdapterNode::OnCmdVel()` 的处理顺序是：

1. 未连接时直接忽略消息；
2. 只读取 `linear.x`、`linear.y`、`angular.z`；
3. 每个轴独立应用 `abs(value) < 1e-4` 的 SI 死区；
4. 调用 `LynxVelocityConverter::Convert()`；
5. 成功发送后更新最后一次有效 `cmd_vel` 的时间。

`linear.z`、`angular.x`、`angular.y` 即使通过入口校验并出现在 `Twist` 中，也不会进入
Lynx `Command=21`；SDK payload 中的 `Z`、`Roll`、`Pitch` 固定为 `0.0`。

### 3.4 SI 限幅与轴比例转换

`LynxVelocityConverter` 对三个轴独立执行限幅和归一化。配置必须满足：

- `max_*` 有限且大于等于 `0`；
- `full_scale_*` 有限且大于 `0`；
- `max_* <= full_scale_*`。

不满足时构造 converter 会抛出 `std::invalid_argument`，adapter 无法正常启动。
输入出现 `NaN` 或无穷值时，converter 拒绝命令，adapter 立即尝试发送零速并清空 watchdog
时间戳。

### 3.5 Lynx 协议下发

`LynxSdkClient::SendMotionCmd()` 再次检查三个比例均为有限值且位于 `[-1, 1]`，但不会裁剪。
最终 JSON 类似：

```json
{
  "PatrolDevice": {
    "Type": 2,
    "Command": 21,
    "Time": "2026-07-21 12:00:00",
    "Items": {
      "X": 0.3500,
      "Y": 0.0000,
      "Z": 0.0,
      "Roll": 0.0,
      "Pitch": 0.0,
      "Yaw": 0.0000
    }
  }
}
```

JSON 前再加 16 字节 Lynx 包头，通过 UDP 发往 `robot_ip:robot_port`。当前默认目标是
`10.21.31.103:30000`。

## 4. 限制参数总表

### 4.1 数值转换参数

下表中的“源码 YAML 当前值”包含当前工作树对 X/Y 上限的未提交调整。

| 参数 | 单位 | 源码 YAML 当前值 | 配置缺失时的代码 fallback | 作用 |
| --- | --- | ---: | ---: | --- |
| `max_linear_x` | `m/s` | `2.0` | `1.5` | X 轴 SI 安全限幅 |
| `max_linear_y` | `m/s` | `2.0` | `1.0` | Y 轴 SI 安全限幅 |
| `max_angular_z` | `rad/s` | `2.0` | `2.0` | Yaw SI 安全限幅 |
| `lynx_full_scale_linear_x_mps` | `m/s` | `2.0` | `2.0` | Lynx `X=1.0` 对应的假定满量程 |
| `lynx_full_scale_linear_y_mps` | `m/s` | `2.0` | `2.0` | Lynx `Y=1.0` 对应的假定满量程 |
| `lynx_full_scale_angular_z_radps` | `rad/s` | `2.0` | `2.0` | Lynx `Yaw=1.0` 对应的假定满量程 |
| adapter 死区 | SI 单位 | `1e-4`，硬编码 | 同左 | 小于该绝对值的单轴置零 |
| SDK 轴范围 | 轴比例 | `[-1.0, 1.0]`，硬编码 | 同左 | 协议最终边界 |
| SDK 序列化精度 | 轴比例 | 4 位小数，硬编码 | 同左 | UDP JSON 数值精度 |

按源码 YAML 当前值，三个轴允许到达的最大协议比例都是 `1.0`：

| 轴 | 最大 SI 输入（限幅后） | full-scale | 最大轴比例 |
| --- | ---: | ---: | ---: |
| X | `2.0 m/s` | `2.0 m/s` | `1.0` |
| Y | `2.0 m/s` | `2.0 m/s` | `1.0` |
| Yaw | `2.0 rad/s` | `2.0 rad/s` | `1.0` |

如果 YAML 没有成功加载，代码 fallback 对应的最大比例是 X=`0.75`、Y=`0.5`、Yaw=`1.0`。

### 4.2 时序与停止参数

| 参数/约束 | 当前值 | 作用 |
| --- | ---: | --- |
| `cmd_vel_timeout_ms` | `500 ms` | 最后一次成功下发后，超时则发送零速 |
| `watchdog_check_interval_ms` | `100 ms` | watchdog 检查周期 |
| Lynx 建议发送频率 | `20 Hz` | 代码注释中的协议建议，不强制执行 |
| `twist_topic_queue_size` | `10` | `remote_controller` publisher 队列深度 |
| Lynx subscription depth | `10` | adapter 订阅队列深度，硬编码 |

watchdog 条件是 `elapsed_ms > cmd_vel_timeout_ms`，并且每 `100 ms` 检查一次。因此默认配置下，
实际触发通常在最后一次成功命令后的约 `500~600 ms`，还需考虑线程调度延迟。

建议按 `20 Hz` 连续发送；一次性发送非零命令后，机器人只会运动到 watchdog 触发为止。

### 4.3 非速度但会影响物理结果的状态

adapter 连接时会发送常规控制模式和站立状态，并支持 walk/trot 步态切换。当前 full-scale 配置不按
模式、运动状态或步态分别标定，因此相同轴比例在不同本体状态下可能得到不同的实际速度或响应。

## 5. 转换示例

以下示例使用当前源码 YAML 的 `max=2.0`、`full_scale=2.0`：

| ROS 输入 | 限幅后 SI 值 | Lynx 轴比例 | UDP JSON |
| --- | ---: | ---: | --- |
| `linear.x=0.7 m/s` | `0.7` | `0.35` | `"X":0.3500` |
| `linear.x=3.0 m/s` | `2.0` | `1.0` | `"X":1.0000` |
| `linear.y=-0.6 m/s` | `-0.6` | `-0.3` | `"Y":-0.3000` |
| `angular.z=1.0 rad/s` | `1.0` | `0.5` | `"Yaw":0.5000` |
| `angular.z=3.0 rad/s` | `2.0` | `1.0` | `"Yaw":1.0000` |

其中 `linear.x=3.0` 能通过 `remote_controller` 的入口校验，但会在 adapter 中被静默裁剪到
`2.0 m/s`。成功响应仍会回显入口值 `3.0`。

## 6. 停止与异常行为

| 场景 | 当前行为 |
| --- | --- |
| WebSocket/MQTT JSON 非法或入口越界 | 不发布新 `Twist`；已有运动由 Lynx watchdog 最终停止 |
| adapter 未连接 | 忽略 `cmd_vel` |
| 直接 ROS 输入为非有限值 | 立即尝试发送零速，并清空 watchdog 时间戳 |
| SDK 发送失败 | 记录错误，不更新时间戳；有历史成功命令时由 watchdog 后续兜底 |
| `cmd_vel` 超时 | 发送 `X=Y=Yaw=0` 并清空时间戳 |
| safe stop / disconnect | 主动发送零速；disconnect 再关闭 UDP socket |

当前没有以下限制或控制：

- 加速度、减速度、jerk 或斜坡限制；
- 速度平滑、低通滤波或防突变处理；
- WebSocket 命令时间戳和新鲜度校验；
- 最大命令频率限制或降采样；
- 基于本体实测速度的闭环限速；
- 按 gait/motion state 切换不同 full-scale；
- 最终 Lynx 轴比例的上游回执。

MQTT 路径使用 FIFO worker queue，当前队列没有长度上限，也不会自动只保留最新速度。在输入积压时，
旧速度命令可能延迟处理；这属于时效风险，不是数值转换。

## 7. 参数调整原则

`max_*` 与 `full_scale_*` 的含义不同：

- `full_scale_*` 应表示 Lynx 轴比例 `1.0` 对应的已标定本体满量程；
- `max_*` 应表示本产品允许的 SI 安全上限。

例如已确认 `X=1.0` 对应约 `2.0 m/s`，但产品只允许 `1.0 m/s`，应配置：

```yaml
max_linear_x: 1.0
lynx_full_scale_linear_x_mps: 2.0
```

这样最大下发比例为 `0.5`。不要为了“限制到 1.0 m/s”把两者同时设成 `1.0`；那会把
`1.0 m/s` 映射为满轴 `X=1.0`，实际物理速度仍可能接近本体满量程。

由于当前没有闭环限速，任何“实际速度不超过某值”的结论都需要在目标 gait、运动状态和负载下做
实机标定。代码中的限制首先保证的是指令比例和假定 SI 映射，不是独立的物理速度安全认证。

## 8. 配置加载与本地运行注意事项

`adapter_lynx` 不直接从源码目录读取 YAML。`AdapterNodeBase` 通过 ament package share 加载：

```text
<adapter_lynx package share>/config/adapter_lynx.yaml
```

`GetParamOrDefault()` 使用的是这份手动解析结果，而不是常规 ROS 参数查询。因此仅修改
`src/adapter_lynx/config/adapter_lynx.yaml`，或仅在命令行传 `-p max_linear_x:=...`，都不能保证
当前运行进程采用新值；需要重新构建/安装并重新 source 环境。

本次核对时，本地 `install/adapter_lynx/share/adapter_lynx/config/adapter_lynx.yaml` 仍是转换修复前的
旧文件，缺少 `max_*` 和 `lynx_full_scale_*`。在该安装树启动时，新 converter 会使用代码 fallback，
不会采用源码 YAML 中尚未安装的 `2.0/2.0` X/Y 上限。

建议每次调整参数后执行：

```bash
source /opt/ros/humble/setup.bash
colcon build --packages-select robot_adapter_interfaces adapter_lynx remote_controller
source install/setup.bash
```

启动 adapter 时检查日志：

```text
cmd_vel SI limits/full-scale: x=.../... m/s, y=.../... m/s, yaw=.../... rad/s
```

该日志比只查看源码 YAML 更能代表当前进程的实际配置。

## 9. 关键代码入口

- WebSocket/MQTT 汇合与 ROS 发布：
  `src/remote_controller/src/remote_controller.cpp`、
  `src/remote_controller/src/velocity_processor.cpp`
- 入口范围校验：`src/remote_controller/src/validator.cpp`
- topic 与配置加载：`src/robot_adapter_interfaces/src/adapter_node_base.cpp`
- adapter 回调与 watchdog：`src/adapter_lynx/src/lynx_adapter_node.cpp`
- SI 限幅和归一化：`src/adapter_lynx/src/lynx_velocity_converter.cpp`
- Command=21 与 UDP 封包：`src/adapter_lynx/src/lynx_sdk_client.cpp`
- Lynx 参数：`src/adapter_lynx/config/adapter_lynx.yaml`
- 转换回归测试：`src/adapter_lynx/test/test_lynx_velocity_converter.cpp`

## 10. 已发现的文档/运行树差异

1. 当前工作树把源码 YAML 的 `max_linear_x/max_linear_y` 从 `1.5/1.0` 调成了 `2.0/2.0`，
   但 converter 代码 fallback 和 `ADAPTER_DEVELOPER_GUIDE.md` 仍是 `1.5/1.0`。
2. 本地 `install/` 中的 adapter YAML 仍是转换修复前版本，需要重新 build/install。
3. `src/remote_controller/API.md` 与 `MQTT_API.md` 已合并为单一文档（`API.md`），速度范围表已核对并更新为与代码一致：`linear_x [-5,5]`、`angular_z [-3.14,3.14]`、其余轴 `[-3.0,3.0]`。

