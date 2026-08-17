## 1. 文档说明

本文档描述 `robot_switch_server` 对外提供的 HTTP 接口，供上位机、前端或其他调用方使用。

后端职责如下：

- 提供统一 HTTP 控制入口
- 管理机器人适配器进程的启动与停止
- 查询当前激活机器人系统信息

---

## 2. 基本信息

### 2.1 服务地址

默认监听地址：

`http://<host>:9098`

默认配置示例：

`http_listen_address: "0.0.0.0:9098"`

`call_timeout_ms` 默认为 `6000`，用于覆盖 Lynx 高层步态操作中的模式、RL 状态和步态串行确认。

### 2.2 数据格式

- 请求协议：HTTP
- 返回格式：`application/json`
- 编码：UTF-8

### 2.3 统一响应结构

所有接口均采用统一的响应包装格式：

```json
{
  "code": 0,
  "msg": "success",
  "data": { ... }
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| code | integer | 结果码，`0` 表示成功，非 `0` 表示错误（见下方错误码表） |
| msg | string | 结果摘要，成功时为操作描述或 `"success"` |
| data | object / null | 业务数据，无数据时为 `null` |

### 2.4 当前已实现接口

| 接口 | 方法 | 说明 |
|---|---|---|
| /health | GET | 服务健康检查 |
| /status | GET | 获取当前服务状态 |
| /start | POST | 启动指定机器人适配器 |
| /stop | POST | 停止当前机器人适配器 |
| /motion | POST | 触发当前机器人声明的离散动作 |
| /system_info | GET | 获取当前激活机器人系统信息 |
| /adapters | GET | 获取服务端已启用的适配器类型列表 |

### 2.5 状态与错误码说明

**state 状态字段**

| 值 | 说明 |
|---|---|
| DISCONNECTED | 当前无机器人适配器运行 |
| CONNECTING | 正在启动适配器并尝试连接机器人 |
| CONNECTED | 已连接，可正常控制 |
| DISCONNECTING | 正在停止适配器 |
| ERROR | 适配器异常退出或处于故障状态 |

**code 错误码**

| code | 名称 | 说明 |
|---|---|---|
| 0 | NONE | 成功，无错误 |
| 1 | UNKNOWN_ROBOT | 未知机器人类型，或类型未在配置中启用 |
| 2 | TARGET_UNAVAILABLE | 目标适配器服务不可达 |
| 3 | BUSY | 当前正在执行切换或停止流程 |
| 4 | CONNECT_FAILED | 启动或连接机器人失败 |
| 5 | DISCONNECT_FAILED | 停止或断开失败 |
| 6 | PRECONDITION_REQUIRED | 前置条件不满足，例如未启动机器人却请求查询系统信息 |
| 400 | BAD_REQUEST | 请求参数缺失或格式错误（同时返回 HTTP 400） |
| 502 | BAD_GATEWAY | 适配器拒绝动作或动作 RPC 调用失败（同时返回 HTTP 502） |

> `last_code` 字段（仅出现在 `/status` 响应的 `data` 中）为字符串类型，取值为上表"名称"列。

---

## 3. HTTP 接口详情

### 3.1 健康检查

#### 接口信息

- 方法：`GET`
- 路径：`/health`

#### 接口说明

用于检查后端服务是否存活。

#### 请求示例

```bash
curl http://localhost:9098/health
```

#### 响应示例

```json
{
  "code": 0,
  "msg": "success",
  "data": null
}
```

---

### 3.2 获取服务状态

#### 接口信息

- 方法：`GET`
- 路径：`/status`

#### 接口说明

返回当前适配器运行状态、最近一次操作结果，以及当前活动适配器健康状态。

#### 请求示例

```bash
curl http://localhost:9098/status
```

#### 响应示例

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "state": "CONNECTED",
    "active_adapter": "go2",
    "busy": false,
    "last_code": "NONE",
    "last_message": "adapter started and connected",
    "last_detail": "adapter_type=go2; pid=12345",
    "adapters": [
      {
        "robot_type": "go2",
        "registered": true,
        "reachable": true,
        "available": true,
        "detail": "GO2 health ok"
      }
    ]
  }
}
```

#### data 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| state | string | 当前状态 |
| active_adapter | string | 当前激活适配器名称，无则为 `"unknown"` |
| busy | boolean | 是否正在处理启动/停止操作 |
| last_code | string | 最近一次操作结果码（字符串名称） |
| last_message | string | 最近一次操作摘要 |
| last_detail | string | 最近一次操作详情 |
| adapters | array | 当前适配器状态列表 |

#### adapters 元素字段

| 字段 | 类型 | 说明 |
|---|---|---|
| robot_type | string | 机器人类型 |
| registered | boolean | 是否已注册到当前运行状态 |
| reachable | boolean | 健康检查服务是否可达 |
| available | boolean | 适配器是否可正常工作 |
| detail | string | 健康检查返回详情 |

---

### 3.3 启动机器人适配器

#### 接口信息

- 方法：`POST`
- 路径：`/start`

#### 接口说明

启动指定机器人类型的适配器进程，并等待适配器健康检查通过后自动执行连接。

当前支持的机器人类型由服务配置项 `enabled_adapter_types` 决定，可通过 `/adapters` 接口查询。

#### 请求参数

| 参数名 | 位置 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| adapter_type | query | string | 是 | 机器人适配器类型，如 `go2` |

#### 请求示例

```bash
curl -X POST "http://127.0.0.1:9098/start" -d "adapter_type=go2"
```

#### 成功响应示例

```json
{
  "code": 0,
  "msg": "adapter started and connected",
  "data": {
    "active_adapter": "go2",
    "state": "CONNECTED",
    "detail": "adapter_type=go2; pid=12345"
  }
}
```

#### 错误响应示例（当前正忙）

```json
{
  "code": 3,
  "msg": "adapter transaction in progress",
  "data": {
    "active_adapter": "unknown",
    "state": "CONNECTING",
    "detail": "state=kStarting"
  }
}
```

#### 错误响应示例（参数缺失，HTTP 400）

```json
{
  "code": 400,
  "msg": "missing required parameter 'adapter_type'",
  "data": null
}
```

#### data 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| active_adapter | string | 操作后当前激活适配器，无则为 `"unknown"` |
| state | string | 操作后当前状态 |
| detail | string | 操作详情或错误细节 |

#### HTTP 状态码

| 状态码 | 说明 |
|---|---|
| 200 | 请求已处理，具体成功失败看 `code` 字段 |
| 400 | 请求参数缺失或格式错误 |

---

### 3.4 停止当前机器人适配器

#### 接口信息

- 方法：`POST`
- 路径：`/stop`

#### 接口说明

停止当前正在运行的机器人适配器。

停止流程包括：

- 调用适配器 `safe_stop`
- 调用适配器 `disconnect`
- 停止适配器进程

#### 请求示例

```bash
curl -X POST http://localhost:9098/stop
```

#### 成功响应示例

```json
{
  "code": 0,
  "msg": "adapter instance stopped",
  "data": {
    "active_adapter": "unknown",
    "state": "DISCONNECTED",
    "detail": "adapter_type=go2"
  }
}
```

#### data 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| active_adapter | string | 操作后当前激活适配器，无则为 `"unknown"` |
| state | string | 操作后当前状态 |
| detail | string | 操作详情或警告信息 |

#### 业务规则

- 如果系统正处于启动或停止中，返回 `code: 3`（BUSY）
- 若没有正在运行的适配器，不报错，直接返回 `code: 0`

---

### 3.5 触发离散 Motion

#### 接口信息

- 方法：`POST`
- 路径：`/motion`

#### 接口说明

触发当前运行中适配器声明的离散动作。服务端不会硬编码动作集合，而是读取当前适配器 `/system_info` 返回中的 `system_info.motions`，然后按 `motion_id` 分发到对应 ROS2 Trigger 服务。

#### 请求参数

| 参数名 | 位置 | 类型 | 必填 | 说明 |
|---|---|---|---|---|
| motion_id | query | string | 是 | 动作 id，必须来自 `GET /system_info` 返回的 `data.system_info.motions[*].id` |

> `display_name` 是纯展示字段，不能拿来当 `motion_id` 传；派发只认 `id`。

#### 调用方式

1. 先启动目标机器人适配器，例如 `go2` 或 `lynx`
2. 调用 `GET /system_info`，读取 `data.system_info.motions`
3. 选择其中一个 `id`
4. 调用 `POST /motion?motion_id=<id>`

> `motion_id` 只允许使用 `[A-Za-z0-9_]+`，不能包含空格、斜杠、短横线或点号。

#### 请求示例

```bash
curl -X POST "http://127.0.0.1:9098/motion?motion_id=stand_up"
```

#### 成功响应示例

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "motion_id": "stand_up",
    "detail": "stand_up success"
  }
}
```

#### 错误响应示例（未知动作）

```json
{
  "code": 400,
  "msg": "unknown motion_id 'lights_on'",
  "data": null
}
```

#### 错误响应示例（适配器拒绝动作或 RPC 失败）

```json
{
  "code": 502,
  "msg": "adapter rejected motion",
  "data": {
    "motion_id": "fail_motion",
    "detail": "forced failure"
  }
}
```

#### data 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| motion_id | string | 本次触发的动作 id |
| detail | string | 适配器返回的原始消息 |

#### 当前已实现 Motion 集

调用方应始终以 `GET /system_info` 的实时返回为准。当前代码中已实现的 motion 如下：

| 适配器 | motion_id | display_name | 说明 |
|---|---|---|---|
| `go2` | `stand_up` | 站立 | 站起，调用 GO2 `RecoveryStand()` |
| `go2` | `stop` | 停止 | 原地停止，调用 GO2 `StopMove()` |
| `go2` | `sit_down` | 趴下 | 趴下，先 `StopMove()` 再 `StandDown()` |
| `go2` | `emergency_stop` | 急停 | 紧急停，调用 GO2 `Damp()` |
| `lynx` | `stand_up` | 站立 | 站起，调用 Lynx `SetMotionState(1)` |
| `lynx` | `soft_stop` | 软急停 | 软急停，调用 Lynx `SetMotionState(2)` |
| `lynx` | `sit_down` | 趴下 | 趴下，调用 Lynx `SetMotionState(4)` |
| `lynx` | `mode_regular` | 常规模式 | 切换常规模式（`ControlUsageMode=0`），速度使用 `Command=21` 轴比例 |
| `lynx` | `mode_navigation` | 导航模式 | 切换导航模式（`ControlUsageMode=1`），速度使用 `Command=25` 绝对值 |
| `lynx` | `rl_control` | RL 控制 | 进入 RL 控制状态（`MotionState=17`） |
| `lynx` | `gait_standard_flat` | 标准平地 | 自动准备 RL 状态并切换标准平地步态，`GaitParam=0x1001` |
| `lynx` | `gait_standard_stairs` | 标准爬楼 | 自动准备 RL 状态并切换标准楼梯步态，`GaitParam=0x1003` |
| `lynx` | `gait_agile_flat` | 敏捷平地 | 必要时自动切导航模式、进入 RL，再切敏捷平地，`GaitParam=0x3002` |
| `lynx` | `gait_agile_stairs` | 敏捷爬楼 | 必要时自动切导航模式、进入 RL，再切敏捷楼梯，`GaitParam=0x3003` |

Lynx 模式、状态和步态控制规则：

- 上层始终以 SI 单位发布 `cmd_vel`（`m/s` 和 `rad/s`），不需要自行转换成 `[-1, 1]`。
- 常规模式下，适配器内部限速并转为 `Command=21` 轴比例；导航模式下，只做 SI 安全限速，然后以 `Command=25` 直接下发绝对速度。
- 辅助模式不接受 `cmd_vel`；状态过期或非 RL 控制状态时，适配器也会拒绝速度指令。
- 四种步态接口是高层串行复合操作：内部会自动进入 RL 控制；由常规模式请求敏捷步态时，还会先自动切到导航模式。辅助模式本身支持敏捷步态，不强制改变。
- 任一中间阶段失败时，adapter 保持零速并返回失败阶段，不自动反向回滚已完成的本体状态转换；重试同一步态请求即可从当前状态继续。
- 模式切换或起立会使机器人重置为基础步态。用户只需在这些操作之后再调用一次目标步态接口，adapter 会重新完成所有前置准备。
- 适配器会等待 `BasicStatus` 中的目标模式、运动状态或步态确认后才返回成功；超时、状态过期或前置条件不满足时返回 HTTP 502。

#### 常见调用示例

GO2：

```bash
curl -X POST "http://127.0.0.1:9098/start?adapter_type=go2"
curl http://127.0.0.1:9098/system_info
curl -X POST "http://127.0.0.1:9098/motion?motion_id=stand_up"
curl -X POST "http://127.0.0.1:9098/motion?motion_id=stop"
curl -X POST "http://127.0.0.1:9098/motion?motion_id=sit_down"
curl -X POST "http://127.0.0.1:9098/motion?motion_id=emergency_stop"
```

Lynx：

```bash
curl -X POST "http://127.0.0.1:9098/start?adapter_type=lynx"
curl http://127.0.0.1:9098/system_info
# 单次调用：必要时由 adapter 自动切导航模式、进入 RL 并等待确认
curl -X POST "http://127.0.0.1:9098/motion?motion_id=gait_agile_flat"
# 可在前一步态切换完成后改为敏捷楼梯
curl -X POST "http://127.0.0.1:9098/motion?motion_id=gait_agile_stairs"
```

#### HTTP 状态码

| 状态码 | 说明 |
|---|---|
| 200 | 动作已成功触发 |
| 400 | 参数缺失、格式非法、未启动适配器或动作不存在 |
| 502 | 适配器拒绝动作，或动作服务调用失败 |

---

### 3.6 获取当前机器人系统信息

#### 接口信息

- 方法：`GET`
- 路径：`/system_info`

#### 接口说明

获取当前激活适配器上报的系统信息。该接口会透传当前机器人适配器 `system_info` 服务返回的数据。

#### 请求示例

```bash
curl http://localhost:9098/system_info
```

#### 成功响应示例

```json
{
  "code": 0,
  "msg": "system_info fetched",
  "data": {
    "active_adapter": "go2",
    "state": "CONNECTED",
    "system_info": {
      "battery": 85,
      "motion": {
        "x": 0.0,
        "y": 0.0,
        "yaw": 0.0
      },
      "motions": [
        {
          "id": "stand_up",
          "service_suffix": "stand_up",
          "description": "Recover to standing posture",
          "display_name": "站立"
        },
        {
          "id": "stop",
          "service_suffix": "stop",
          "description": "Halt in place",
          "display_name": "停止"
        },
        {
          "id": "sit_down",
          "service_suffix": "sit_down",
          "description": "Stop motion then stand down",
          "display_name": "趴下"
        },
        {
          "id": "emergency_stop",
          "service_suffix": "emergency_stop",
          "description": "Damp all joints",
          "display_name": "急停"
        }
      ],
      "details": {
        "connected": true,
        "network_interface": "eth0",
        "has_low_state": true,
        "has_sport_state": true,
        "sport_fresh": true
      }
    }
  }
}
```

#### 错误响应示例（无运行中适配器）

```json
{
  "code": 6,
  "msg": "no running adapter instance",
  "data": {
    "active_adapter": "unknown",
    "state": "DISCONNECTED",
    "system_info": null
  }
}
```

#### data 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| active_adapter | string | 当前激活适配器，无则为 `"unknown"` |
| state | string | 当前系统状态 |
| system_info | object / null | 适配器返回的系统信息，失败时为 `null` |

#### 说明

- `system_info` 为适配器透传 JSON，对象顶层统一包含 `battery`、`motion`、`details`
- 若适配器声明了离散动作，还会额外包含 `motions` 数组
- `motion` 表示当前速度状态 `{x, y, yaw}`，不是离散动作名
- `motions[*].display_name` 是给用户看的中文短名。基于 `SystemInfoBuilder` 构建 `/system_info`（本仓库内所有适配器均如此）时该字段保证非空——适配器未显式提供时，`SystemInfoBuilder::SetMotions` 会自动回落成 `id`；但这个保证来自适配器进程内部，`robot_switch_server` 只是把适配器返回的原始 JSON 透传给前端，并不会重新校验或补全该字段。若接入未使用 `SystemInfoBuilder`、自行拼接 `/system_info` JSON 的第三方适配器，`display_name` 键可能整体缺失，前端仍应保留一行回落到 `id` 的兜底逻辑
- `motions[*].description` 是面向开发者的英文长描述，不适合直接展示给用户
- 调用 `/motion` 前，应先读取 `system_info.motions[*].id`
- 不同适配器返回的 `details` 内容不同，调用方应按对象动态解析

---

### 3.7 获取已启用适配器类型

#### 接口信息

- 方法：`GET`
- 路径：`/adapters`

#### 接口说明

返回服务端配置中 `enabled_adapter_types` 的列表，即当前允许通过 `/start` 接口启动的机器人适配器类型。

#### 请求示例

```bash
curl http://localhost:9098/adapters
```

#### 响应示例

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "enabled_adapter_types": ["go2", "m20"]
  }
}
```

#### data 字段说明

| 字段 | 类型 | 说明 |
|---|---|---|
| enabled_adapter_types | array<string> | 当前配置中启用的适配器类型名称列表 |

#### 说明

- 列表内容由服务启动时读取的 `server.yaml` 中 `enabled_adapter_types` 决定，运行期间不会变化
- 列表按字母序排列
- 可在调用 `/start` 前查询此接口，获取合法的 `adapter_type` 取值范围
