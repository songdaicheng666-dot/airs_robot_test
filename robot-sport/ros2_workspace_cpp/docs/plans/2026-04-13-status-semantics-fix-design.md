# `/status` 接口状态语义修正设计

日期：2026-04-13

## 1. 背景与修改目标

当前 `robot_switch_server` 的 `/status` 接口存在明显语义偏差：

- 顶层 `state` 由 `AdapterRuntimeManager` 内部状态机直接导出，本质上表示的是“当前 adapter session 是否处于 running”
- 它并不直接表示“设备当前是否真实在线、是否仍可控”
- 因此在 adapter 进程仍存活、ROS2 服务仍可达、但机器人本体已经断链时，接口仍可能返回 `CONNECTED`

这与调用方对顶层 `state` 的直觉预期不一致。对上层系统来说，`state` 应该首先回答“当前激活设备是否连着”，而不是“后台 adapter 进程是不是还在”。

本次修改的目标是：

1. 修正 `/status` 顶层 `state` 的语义，使其表示真实设备连接状态
2. 保持现有字段集合不扩张，不新增兼容字段
3. 在设备掉线但 adapter 进程仍活着时，顶层 `state` 返回 `DISCONNECTED`
4. 统一 `lynx`、`go2` 的健康判断标准，消除旧缓存、状态陈旧、假阳性健康的影响

## 2. 现状问题与根因分析

### 2.1 `state` 当前表示的是运行态，不是设备态

`AdapterRuntimeManager::SnapshotStateLocked()` 直接将内部状态机映射到 `SwitchState`：

- `Idle -> DISCONNECTED`
- `Starting -> CONNECTING`
- `Running -> CONNECTED`
- `Stopping -> DISCONNECTING`
- `Faulted -> ERROR`

这套映射的问题在于：`Running` 只说明 adapter 已启动并完成过一次连接流程，不说明设备此刻仍然在线。

### 2.2 健康检查不会回写顶层 `state`

`GetStatusWithHealth()` 的现有逻辑是（`adapter_runtime_manager.cpp:329-357`）：

1. 加锁后先调用 `RefreshCrashedProcessState()` 检查进程崩溃（崩溃时会将状态机转为 `Faulted`，因此进程级故障能正确反映在快照中）
2. 再构造基于状态机的快照（`SnapshotStateLocked()`）
3. 释放锁后调用 adapter 的 `/health`
4. 把健康结果写入 `snapshot.adapters[active_adapter]`

但顶层 `snapshot.state` 不会因为 `/health` 失败而降级。结果就是：

- `adapters[].available=false`
- 但 `state` 仍然是 `CONNECTED`

这会逼迫所有调用方绕过顶层状态，自行组合 `state + adapters[].available + detail` 来推断真实连接情况，接口主状态字段失去了意义。

> 注：进程崩溃能正确映射为 `ERROR`，正是因为 `RefreshCrashedProcessState()` 在快照之前执行。本次修改需要解决的是"进程活着但设备掉线"的场景。

### 2.3 `lynx` 的健康检查过宽松

`adapter_lynx` 当前存在两个问题：

1. `OnHealth()` 无条件 `response->success = true`（`lynx_adapter_node.cpp:164`），不检查 `connected_` 状态，也不检查 `status.valid`
2. `LynxSdkClient` 的状态缓存一旦变为 `valid=true`（`lynx_sdk_client.cpp:453`），后续没有 stale 失效逻辑；`last_update` 字段虽然被写入（`lynx_sdk_client.cpp:454`），但在整个代码库中从未被读取或用于过期判断

这意味着：

- 机器人已经不在线时，`/health` 仍可能返回成功
- 旧 session 的状态缓存如果未清空，甚至可能误导下次 connect 成功

### 2.4 `go2` 的健康检查也有同类问题

`adapter_go2` 当前的问题形态略有不同，但本质一致：

1. `OnHealth()` 已经调用了 `GetServiceList()`（`go2_adapter_node.cpp:273`），但仅将返回值写入 JSON 响应体，不用来判定 `success`；最终无条件 `response->success = true`（`go2_adapter_node.cpp:336`）。注意：`OnHealth()` 有 `EnsureSdkInitialized()` 前置检查（`go2_adapter_node.cpp:268`），SDK 未初始化时会提前返回 `success=false`，但这只覆盖进程级异常，不覆盖设备掉线
2. 仅记录 `has_sport_state_` / `has_low_state_` 这类”曾经收到过”的布尔值（`go2_adapter_node.hpp:80-81`），且这些标志一旦在回调中被设为 `true`（`go2_adapter_node.cpp:642,648`），在 disconnect/reconnect 周期中从未被重置回 `false`
3. 没有状态时间戳，也没有 stale 判定

结果是：

- 只要曾经收到过状态，就可能长期被当作健康，即使跨越 connect 周期
- 设备真实掉线后，健康状态不会自动退化

## 3. 为什么要直接修改现有 `state` 语义

本次方案明确选择“不新增 `runtime_state`，直接修正现有 `state`”。

原因如下：

### 3.1 用户关注的是主状态字段的真实性

当前问题不是“缺少更多调试字段”，而是“主字段说错了话”。如果继续保留旧语义，只要求调用方改读 `adapters[].available`，那么误用会长期存在。

### 3.2 顶层 `state` 应承担最直接的判定职责

对于绝大多数调用方，接口使用方式都是：

- 先看 `state`
- 再决定 UI 呈现、操作按钮可用性、控制链路状态

因此 `state` 必须优先反映“设备是否连着”，否则这个字段本身就不值得保留。

### 3.3 进程态不是本接口最重要的信息

adapter session 是否仍在，对排障是有价值的，但对上层业务的第一优先级不是“session 活没活”，而是“设备还能不能用”。

因此本次取舍是：

- 顶层 `state`：表达设备态
- `active_adapter`、`last_code`、`last_message`、`last_detail`：保留排障信息

## 4. 为什么设备掉线时映射为 `DISCONNECTED`

本次方案明确将“adapter 进程还活着，但设备已断链/状态 stale”的场景映射为 `DISCONNECTED`，而不是 `ERROR`。

原因如下：

### 4.1 用户语义更直观

从设备视角看，掉线就是“未连接”。对上层使用者来说，看到 `DISCONNECTED` 的理解成本最低，也最符合操作预期。

### 4.2 `ERROR` 应保留给生命周期级故障

`ERROR` 更适合表达如下情况：

- adapter 子进程崩溃
- 状态机进入 faulted
- 启停流程出现不可恢复错误

如果把“设备暂时掉线”也统一映射成 `ERROR`，会把“设备连接问题”和“进程级故障”混在一起，弱化问题分层。

### 4.3 掉线时仍可通过明细保留排障信息

即使顶层 `state=DISCONNECTED`，仍然可以通过以下字段知道这是“掉线而不是从未启动”：

- `active_adapter`
- `last_code`
- `last_message`
- `last_detail`
- `adapters[active_adapter].reachable`
- `adapters[active_adapter].available`
- `adapters[active_adapter].detail`

因此本次修改不会牺牲可排障性，只是让顶层语义回归正确。

## 5. 最终设计方案

### 5.1 顶层 `state` 的新语义

`SwitchStatusSnapshot.state` 调整为对外设备连接态，定义如下：

- `DISCONNECTED`
  - 没有 active adapter
  - 或 active adapter 当前 `Health()` 不可达
  - 或 active adapter `Health().ok == false`
  - 或 adapter 内部状态已 stale，不再认为设备在线
- `CONNECTING`
  - adapter 启动中或连接中
- `CONNECTED`
  - active adapter 存在，且 `Health().reachable == true` 且 `Health().ok == true`
- `DISCONNECTING`
  - adapter 停止中
- `ERROR`
  - adapter 生命周期故障，例如进程崩溃、状态机 faulted

### 5.2 `GetStatusWithHealth()` 的新派生规则

`AdapterRuntimeManager::GetStatusWithHealth()` 改为两阶段构造：

1. 先生成基于状态机的原始快照
2. 再根据 `Health()` 结果派生对外 `state`

具体规则：

- 原始状态为 `Idle` / `ShuttingDown`：返回 `DISCONNECTED`
- 原始状态为 `Starting`：返回 `CONNECTING`
- 原始状态为 `Stopping`：返回 `DISCONNECTING`
- 原始状态为 `Faulted`：返回 `ERROR`
- 原始状态为 `Running`：
  - `health.reachable && health.ok` -> `CONNECTED`
  - 否则 -> `DISCONNECTED`

### 5.3 `last_*` 字段的处理原则

顶层 `last_code` / `last_message` / `last_detail` 保留现有机制，但在 `GetStatusWithHealth()` 的返回快照中允许做一次响应级覆盖：

- `/health` 服务不可达时：
  - `last_code = TARGET_UNAVAILABLE`
  - `last_message = "adapter health service unreachable"`
  - `last_detail` 透传 health 错误消息
- `/health` 可达但设备不健康时：
  - `last_code = TARGET_UNAVAILABLE`
  - `last_message = "active adapter disconnected"`
  - `last_detail` 透传 adapter 细节

这里的覆盖只作用于本次响应快照，不强制写回 manager 的持久 `last_*` 状态，避免查询接口污染操作结果。

### 5.4 启动流程与健康语义解耦

一旦 `/health.ok` 被定义为“设备当前真实在线”，启动流程就不能再用它判断“是否可以开始 connect”。

因此 `WaitForAdapterReadyAndConnect()`（`adapter_runtime_manager.cpp:463-497`）需要调整。当前代码（行 476）为：

```cpp
if (!health.reachable || !health.ok) {
    // ... retry
    continue;
}
```

修改为：

- 只要求 `health.reachable == true`，说明 ROS2 `/health` 服务已经起来
- 不要求 `health.ok == true`
- 然后直接调用 `Connect()`

否则会形成循环依赖：

- 设备尚未 connect
- `/health.ok` 因此为 false
- manager 又因为 `/health.ok=false` 而拒绝发起 `Connect()`

### 5.5 `adapter_lynx` 的修正方案

`adapter_lynx` 需要把“是否连着”从“曾经连上过”修成“现在仍然在线”。

具体修改：

1. 增加参数：
   - `status_stale_timeout_ms`，默认 `2000`
2. 在 `LynxSdkClient::Initialize()`、`Shutdown()` 和每次 `OnConnect()` 开始前重置 `latest_status_` 为默认值（其内部 `valid` 归 `false`，`last_update` 归零），确保旧 session 的缓存不会残留到新 session。`OnConnect()` 开始前的清空是关键点——必须在重新订阅状态之前执行
3. `OnConnect()` 仅接受”本次 connect 启动之后收到的 fresh status”
4. `OnHealth()` 逻辑改为：
   - `connected_ == true`
   - `status.valid == true`
   - `now - status.last_update <= status_stale_timeout_ms`
   - 三者同时满足时 `response->success = true`
5. `OnSystemInfo()` 与健康结果对齐：
   - disconnected 或 stale 时返回失败
   - 详情中保留 `connected`、`valid`、`stale_ms`、`last_error`

这样做的原因是：

- `lynx` 当前最危险的问题是旧缓存可能跨 session 残留
- 不清空缓存，新的连接流程和新的健康判定都会被旧状态污染

### 5.6 `adapter_go2` 的修正方案

`adapter_go2` 需要从“曾经收到过状态”切换为“最近持续收到 fresh 状态”。

具体修改：

1. 增加参数：
   - `connect_state_timeout_ms`，默认 `2000`
   - `state_stale_timeout_ms`，默认 `2000`
   - 注：使用 `_ms` 后缀而非 `_sec`，与现有 `cmd_vel_timeout_ms` 保持一致；stale 检测需要毫秒精度
2. 新增状态时间戳：
   - `last_sport_state_time_`
   - `last_low_state_time_`
3. connect 开始时清空：
   - `has_sport_state_`
   - `has_low_state_`
   - `last_sport_state_time_`
   - `last_low_state_time_`
4. `OnConnect()` 成功条件改为：
   - SDK 初始化成功
   - 必要控制命令成功
   - 且在 connect 发起后收到至少一帧 fresh `sport_state`
5. `OnHealth()` 逻辑改为（保留现有 `EnsureSdkInitialized()` 前置检查）：
   - `connected_ == true`
   - `GetServiceList()` 返回成功（当前代码已在 `OnHealth()` 中调用 `GetServiceList()`（`go2_adapter_node.cpp:273`），但返回值仅写入 JSON 不参与 `success` 判定；修改后需将其作为硬门槛）
   - `sport_state` 在 `state_stale_timeout_ms` 内
   - 满足以上条件时 `response->success = true`
6. `low_state` 仅作为补充信息，不作为硬门槛
   - 原因：`sport_state` 更直接代表控制链路活性
   - 如果把 `low_state` 也设为硬门槛，实机会更容易出现误判掉线
7. `OnSystemInfo()` 规则：
   - disconnected 或 `sport_state` stale 时返回失败
   - `low_state` 若缺失，可返回部分成功，但电池字段不填，并在 details 中标记缺失

## 6. 公共接口与文档影响

本次修改会影响现有对外接口的语义，但不新增字段、不改 JSON 结构。

### 6.1 对外变化

`/status` 返回结构保持不变，但 `data.state` 的语义改变：

- 修改前：表示 adapter 运行态
- 修改后：表示设备连接态

### 6.2 不变项

以下字段保持现有结构：

- `active_adapter`
- `busy`
- `last_code`
- `last_message`
- `last_detail`
- `adapters[]`

### 6.3 文档同步要求

需要同步更新：

- `robot_adapter_interfaces` 开发文档中 `/health` 语义
- `robot_switch_server` 对 `/status` 的接口说明

重点明确两点：

1. 顶层 `state` 现在表示设备态
2. adapter `/health.success` 现在表示“设备当前健康可控”，不是“节点进程还在”

## 7. 实施顺序

建议按以下顺序实施，避免中间状态出现语义冲突：

### 阶段一：先修 `robot_switch_server`

1. 调整 `GetStatusWithHealth()` 的状态派生逻辑
2. 调整 `WaitForAdapterReadyAndConnect()`，只以 `health.reachable` 作为启动前提
3. 保证在 adapter 尚未修复前，顶层逻辑不会和新的健康语义互相打架

### 阶段二：修 `adapter_lynx`

1. 加 stale timeout
2. 清空缓存
3. 收紧 `OnHealth()`
4. 收紧 `OnSystemInfo()`

### 阶段三：修 `adapter_go2`

1. 新增状态时间戳
2. connect 依赖 fresh `sport_state`
3. 收紧 `OnHealth()`
4. 收紧 `OnSystemInfo()`

### 阶段四：补测试与文档

1. 为 `robot_switch_server`、`adapter_lynx`、`adapter_go2` 增加测试入口
2. 补足单元测试和必要手测步骤
3. 更新接口与开发文档

## 8. 测试与验收标准

当前相关包几乎没有现成测试覆盖，因此本次修改必须先补最小测试能力，再验证行为。

### 8.1 单元测试场景

`robot_switch_server`

- `Running + health ok -> CONNECTED`
- `Running + health unreachable -> DISCONNECTED`
- `Running + health reachable but ok=false -> DISCONNECTED`
- `Faulted -> ERROR`
- 启动阶段 `health.reachable=true, health.ok=false` 时仍允许继续 `Connect()`

## 9. 风险与取舍

### 9.1 兼容性风险

本次方案不新增 `runtime_state`，而是直接修正现有 `state` 语义。这意味着：

- 依赖旧语义的调用方需要适配
- 如果有调用方曾把 `CONNECTED` 理解为“adapter 进程已启动”，修改后会看到行为变化

这是有意为之的取舍。因为当前的主要问题正是旧语义本身不正确，继续保留只会使误用长期固化。

### 9.2 stale timeout 参数需要实机校准

默认 `2000ms` 是一个合理起点，但仍需通过实机观测确认：

- 如果过短，可能把短暂抖动误判为掉线
- 如果过长，状态退化会显得迟钝

因此超时值应作为参数暴露，而不是写死在代码里。

## 10. 最终结论

本次修改的核心不是“再补一个健康字段”，而是把 `/status` 的主状态字段修回正确语义：

- `state` 表示设备现在是否真实连接
- 掉线时返回 `DISCONNECTED`
- 进程级故障时返回 `ERROR`

只有这样，顶层状态才重新具备直接可用性，调用方才不需要通过拼装多个次级字段来推导最基本的连接结论。
