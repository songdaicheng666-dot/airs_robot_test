# Remote Controller API 文档

## 目录

- [概述](#概述)
- [连接信息](#连接信息)
- [配置](#配置)
- [请求格式](#请求格式)
- [响应格式](#响应格式)
- [错误码](#错误码)
- [客户端使用指南](#客户端使用指南)
- [示例代码](#示例代码)
- [已知限制](#已知限制)

## 概述

`remote_controller` 节点通过两条独立的传输通道接收机器人速度控制命令：

- **WebSocket**：默认端口 9099，始终启用
- **MQTT**：默认关闭，`mqtt.enabled=true` 时启动订阅器

两条通道共用同一套消息处理链路（`MessageValidator` → `VelocityProcessor` → ROS2 `cmd_vel` 发布 → `ResponseBuilder`），因此请求体、响应体结构、校验规则和错误码在两种协议下完全一致，仅传输方式和连接方式不同。本文统一描述两者共享的行为，并分别说明各自的连接细节。

节点即使 MQTT 启动失败，仍会继续提供 WebSocket 服务；反之 WebSocket 启动失败会导致节点抛出异常退出。

## 连接信息

### WebSocket

```
协议: WebSocket (ws://，非 TLS)
默认端口: 9099
默认监听地址: 0.0.0.0
完整 URL: ws://localhost:9099
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `websocket.port` | `9099` | 监听端口 |
| `websocket.host` | `0.0.0.0` | 绑定的网络接口 |
| `websocket.max_connections` | `10` | 同时允许的客户端连接数，超出时新连接会被 `policy_violation` 关闭 |

```javascript
const ws = new WebSocket('ws://localhost:9099');

ws.onopen = () => console.log('WebSocket 连接已建立');
ws.onmessage = (event) => console.log('收到响应:', JSON.parse(event.data));
ws.onerror = (error) => console.error('WebSocket 错误:', error);
ws.onclose = (event) => console.log('连接已关闭:', event.code, event.reason);
```

### MQTT

服务端使用 Eclipse Paho MQTT C++ `async_client`，启用自动重连，并在重连后重新订阅下行主题。

主题格式：

```text
sys/{region}/{tenant_id}/{hub_id}/remote_control/downlink   # 客户端发布控制命令
sys/{region}/{tenant_id}/{hub_id}/remote_control/uplink     # 客户端订阅处理响应
```

| 字段 | 来源 |
|------|------|
| `region` | `mqtt.region` 配置项 |
| `tenant_id` | `mqtt.tenant_id` 配置项 |
| `hub_id` | `/workspace/.info/device_info.json` 中的 `SN` 字段 |

示例（`region=cn-sz`, `tenant_id=gs`, `hub_id=ROBOT001`）：

```text
downlink: sys/cn-sz/gs/ROBOT001/remote_control/downlink
uplink:   sys/cn-sz/gs/ROBOT001/remote_control/uplink
```

服务端连接 Broker 使用的 Client ID 固定为 `remote_controller_{hub_id}`；客户端可使用任意唯一 Client ID。

重要说明：

- `region` 和 `tenant_id` 应视为必填部署参数，但当前实现在启动时不会校验它们是否为空——留空时主题会变成 `sys///<hub_id>/remote_control/...`，通常不是期望的生产主题。

## 配置

### 加载顺序

1. 内置默认值
2. 配置文件：`REMOTE_CONTROLLER_CONFIG` 指定的文件，或默认 `config/remote_controller_config.json`
3. 环境变量覆盖
4. 设备信息文件中的 `SN` 覆盖 `ros.hub_id`（唯一来源，配置文件和环境变量均不生效）

### 配置项

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `websocket.port` | `9099` | WebSocket 监听端口 |
| `websocket.host` | `0.0.0.0` | WebSocket 绑定地址 |
| `websocket.max_connections` | `10` | 最大并发连接数 |
| `ros.twist_topic_queue_size` | `10` | `cmd_vel` 发布队列长度 |
| `ros.hub_id` | `DEFAULT_HUB_ID` | 仅在读取 `SN` 失败时使用的回退值 |
| `logging.level` | `INFO` | 日志级别 |
| `logging.enable_websocket_logs` | `true` | 是否记录 WebSocket 连接日志 |
| `mqtt.enabled` | `false` | 是否启用 MQTT |
| `mqtt.broker` | `tcp://localhost:1883` | Broker URI |
| `mqtt.region` | `""` | 主题中的区域标识 |
| `mqtt.tenant_id` | `""` | 主题中的租户标识 |
| `mqtt.username` | `""` | Broker 用户名 |
| `mqtt.password` | `""` | Broker 密码 |
| `mqtt.qos` | `1` | 订阅和响应发布使用的 QoS |
| `mqtt.keep_alive_interval` | `20` | Keep Alive，单位秒 |
| `mqtt.clean_session` | `true` | 是否使用 clean session |

可用的环境变量覆盖项：

| 环境变量 | 对应配置 |
|----------|----------|
| `TWIST_TOPIC_QUEUE_SIZE` | `ros.twist_topic_queue_size` |
| `MQTT_BROKER` | `mqtt.broker` |
| `MQTT_REGION` | `mqtt.region` |
| `MQTT_TENANT_ID` | `mqtt.tenant_id` |
| `MQTT_USERNAME` | `mqtt.username` |
| `MQTT_PASSWORD` | `mqtt.password` |

说明：

- 当前实现不会从环境变量覆盖 `websocket.*`、`mqtt.enabled`、`mqtt.qos`、`mqtt.keep_alive_interval`、`mqtt.clean_session`。
- 只有当 `mqtt.username` 非空时，代码才会同时设置用户名和密码；只配置 `password` 不会启用认证。
- `HUB_ID` 环境变量和配置文件中的 `ros.hub_id` 均**不会**生效，`hub_id` 只从设备信息文件读取。

## 请求格式

### 基本要求

- 消息体必须是 JSON 对象，编码为 UTF-8
- WebSocket：作为文本帧发送；MQTT：发布到 `downlink` 主题（推荐 QoS 1）

### 速度控制命令

```json
{
  "linear_x": 1.0,
  "angular_z": 0.5,
  "linear_y": 0.0,
  "linear_z": 0.0,
  "angular_x": 0.0,
  "angular_y": 0.0
}
```

| 字段 | 类型 | 必需 | 范围 | 单位 | 默认值 |
|------|------|------|------|------|--------|
| `linear_x` | number | 是 | `[-5.0, 5.0]` | m/s | 无 |
| `linear_y` | number | 否 | `[-3.0, 3.0]` | m/s | `0.0` |
| `linear_z` | number | 否 | `[-3.0, 3.0]` | m/s | `0.0` |
| `angular_x` | number | 否 | `[-3.0, 3.0]` | rad/s | `0.0` |
| `angular_y` | number | 否 | `[-3.0, 3.0]` | rad/s | `0.0` |
| `angular_z` | number | 是 | `[-3.14, 3.14]` | rad/s | 无 |

为保持向后兼容，只提供 `linear_x` 和 `angular_z` 即可（对应标准差分驱动机器人控制模式），其余字段自动补 `0.0`。

### 校验规则

校验按以下顺序执行，任一阶段失败都不会发布 `cmd_vel`：

1. **JSON 结构**：必须能解析为 JSON 对象；解析失败或不是对象直接返回 `1004`
2. **必需字段**：`linear_x`、`angular_z` 必须存在
3. **字段类型**：所有已提供字段（含可选字段）都必须是 JSON 数字，不能是字符串
4. **数值范围**：仅当第 3 步全部字段类型都合法时才会检查；每个已提供字段必须落在对应范围内

内部会收集同一阶段的多条错误，但最终响应**只返回第一条**（按 `linear_x, linear_y, linear_z, angular_x, angular_y, angular_z` 的字段顺序，必需字段错误优先于类型错误，类型错误优先于范围错误）。

### 有效示例

```json
{"linear_x": 1.5, "angular_z": 0.8}
```

```json
{
  "linear_x": 1.0,
  "linear_y": 0.5,
  "linear_z": 0.0,
  "angular_x": 0.0,
  "angular_y": 0.0,
  "angular_z": 1.2
}
```

停止命令：

```json
{"linear_x": 0.0, "angular_z": 0.0}
```

### 无效示例

```json
// 缺少必需字段 angular_z → code 1001
{"linear_x": 1.0}
```

```json
// linear_x 是字符串而非数字 → code 1002
{"linear_x": "1.0", "angular_z": 0.5}
```

```json
// linear_x 超出 [-5.0, 5.0] 范围 → code 1003
{"linear_x": 10.0, "angular_z": 0.5}
```

```json
// 语法错误（多余逗号）→ code 1004
{"linear_x": 1.0, "angular_z": 0.5,}
```

## 响应格式

处理结果通过同一通道返回：WebSocket 直接回发文本帧；MQTT 发布到 `uplink` 主题。响应始终是以下结构：

```json
{
  "code": 0,
  "msg": "success",
  "data": {},
  "requestId": "req_483921"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `code` | int | `0` 表示成功，非 0 为错误码（见[错误码](#错误码)） |
| `msg` | string | 成功固定为 `"success"`；失败为人类可读的错误信息 |
| `data` | object | 成功时为 `{velocity, target}`；失败时为 `{error_details}` |
| `requestId` | string | 服务端随机生成，格式为 `req_` + 6 位随机数字（如 `req_483921`），与客户端请求中的任何 ID 无关 |

### 成功响应

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "velocity": {
      "linear_x": 1.5,
      "linear_y": 0.0,
      "linear_z": 0.0,
      "angular_x": 0.0,
      "angular_y": 0.0,
      "angular_z": 0.8
    },
    "target": {
      "topic": "/ROBOT001/cmd_vel",
      "hub_id": "ROBOT001"
    }
  },
  "requestId": "req_483921"
}
```

### 错误响应

```json
{
  "code": 1001,
  "msg": "Required field 'angular_z' is missing",
  "data": {
    "error_details": {
      "code": 1001,
      "message": "Required field 'angular_z' is missing",
      "field": "angular_z",
      "suggestion": "Include \"angular_z\": <number> in your JSON request"
    }
  },
  "requestId": "req_271604"
}
```

`error_details.field` 和 `error_details.suggestion` 只在有值时出现。

## 错误码

| `code` | 含义 |
|--------|------|
| `0` | 成功 |
| `1001` | 缺少必需字段 |
| `1002` | 字段类型错误 |
| `1003` | 数值超出范围 |
| `1004` | JSON 格式错误 |
| `2001` | ROS 消息发布失败 |
| `5001` | 内部处理错误 |

说明：

- 代码中还定义了 `2002`（连接错误）、`5002`（服务过载），但当前处理链路不会主动返回这两个错误码。
- 传输层问题（Broker 连接失败、订阅失败、启动超时等）只会出现在节点日志里；因为请求尚未进入业务处理链路，不会产生业务响应。

## 客户端使用指南

### WebSocket

- 消息作为 JSON 文本帧发送，每次连接可连续发送多条命令
- 达到 `max_connections` 后，新连接会被以 `policy_violation` 关闭
- 当前实现未强制消息大小或心跳间隔限制

### MQTT

- 先订阅 `uplink`，再发布 `downlink`
- 客户端和服务端尽量统一使用 QoS 1
- 每个客户端使用唯一的 Client ID，避免被 Broker 踢下线
- 上线前确认 `region`、`tenant_id`、`hub_id` 三段完全一致
- 长时间收不到响应时，优先检查：节点是否启用了 MQTT、Broker 地址是否正确、主题是否拼错、`region`/`tenant_id` 是否为空

仓库内提供的测试脚本：

- `test/test_responses.py` — WebSocket 自动化校验 + 交互式键盘控制
- `test/test.py` — WebSocket 20Hz、持续 10 秒的负载测试
- `test/test_mqtt.py` — MQTT 简单测试脚本

## 示例代码

### WebSocket（JavaScript）

```javascript
const ws = new WebSocket('ws://localhost:9099');

ws.onopen = () => {
  ws.send(JSON.stringify({ linear_x: 1.0, angular_z: 0.5 }));
};

ws.onmessage = (event) => {
  console.log(JSON.parse(event.data));
};
```

### MQTT（Python）

```python
import json
import paho.mqtt.client as mqtt

BROKER = "localhost"
PORT = 1883
REGION = "cn-sz"
TENANT_ID = "gs"
HUB_ID = "ROBOT001"

DOWNLINK = f"sys/{REGION}/{TENANT_ID}/{HUB_ID}/remote_control/downlink"
UPLINK = f"sys/{REGION}/{TENANT_ID}/{HUB_ID}/remote_control/uplink"


def on_connect(client, userdata, flags, rc):
    if rc == 0:
        client.subscribe(UPLINK, qos=1)
    else:
        print(f"connect failed: rc={rc}")


def on_message(client, userdata, msg):
    print(json.loads(msg.payload.decode()))


client = mqtt.Client(client_id="example_controller")
client.on_connect = on_connect
client.on_message = on_message

client.connect(BROKER, PORT, keepalive=60)
client.loop_start()

client.publish(DOWNLINK, json.dumps({"linear_x": 1.0, "angular_z": 0.5}), qos=1)
```

### MQTT（mosquitto CLI）

```bash
mosquitto_sub -h localhost -t "sys/cn-sz/gs/ROBOT001/remote_control/uplink" -v
```

```bash
mosquitto_pub -h localhost \
  -t "sys/cn-sz/gs/ROBOT001/remote_control/downlink" \
  -m '{"linear_x": 1.0, "angular_z": 0.5}'
```

## 已知限制

- 无身份认证；生产环境应通过网络隔离、防火墙规则或反向代理增加安全层。WebSocket 目前只支持非 TLS 连接（`ws://`），无 WSS 选项。
- 响应中不包含 `timestamp`、`version`、`processing_time_ms` 等字段；`requestId` 与客户端无关，纯服务端生成。
- MQTT 的 `region`/`tenant_id`/`enabled` 等关键参数在启动时不做校验，配置错误只能通过日志排查。
