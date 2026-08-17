# 适配器生命周期与运动控制重构设计

日期：2026-03-15

## 1. 背景与目标

本次设计基于对 `ros2_workspace_cpp` 全仓的静态审查，重点围绕以下两个目标展开：

1. 修复 `robot_switch_server` 对适配器子进程的生命周期管理缺陷，确保不会遗留僵尸进程、孤儿进程，且崩溃、关闭、重复启停等路径行为可预测。
2. 修复 `adapter_go2` 当前运动控制链路中的安全问题，确保在上位机失联、命令超时、服务调用失败等情况下，系统能够自动收敛到安全状态。

本次重构允许：

- 调整内部模块边界
- 调整标准服务契约
- 调整 HTTP 参数与返回结构
- 调整进程启动/退出方式
- 做中到大范围代码重构

本次重构不建议：

- 将硬件 SDK 直接并入 `robot_switch_server` 进程
- 在未建立统一监督模型之前继续增补新适配器

## 2. Code Review 结论摘要

### 2.1 总体结论

当前实现不满足“生命周期严格正确”和“实机运动控制可接受”的要求。现有代码具备基本结构，但在以下关键点上存在阻断级问题：

- 子进程退出检测依赖缓存路径，存在僵尸进程风险
- 主进程退出不清理活动适配器，存在孤儿进程风险
- `AdapterClient` 与当前 ROS2 executor 使用方式冲突
- `AdapterRuntimeManager` 状态机并发写入不严谨
- Go2 运动控制没有 `cmd_vel` watchdog / deadman
- 标准服务契约不完整，客户端假设与适配器实现不一致

### 2.2 分级问题清单

#### P1 - High

1. `src/robot_switch_server/src/core/adapter_runtime_manager.cpp`
   - `GetStatusWithHealth()` 在 `snapshot_valid_` 为真时直接返回缓存，绕过 `RefreshCrashedProcessState()`。
   - 结果：子进程异常退出后可能长期不被 `waitpid` 回收，形成 zombie，状态还可能保持假阳性。

2. `src/robot_switch_server/src/main.cpp`
   - 主进程退出路径没有主动 `Stop()` 活动适配器。
   - 当前子进程又在 `fork()` 后调用 `setsid()`，导致活动适配器在父进程结束后会继续存活，形成 orphan。

3. `src/robot_adapter_interfaces/src/adapter_client.cpp`
   - `AdapterClient::CallTrigger()` 直接在共享 node 上调用 `rclcpp::spin_until_future_complete()`。
   - 该 node 已在主流程中被 `rclcpp::spin(node)` 驱动，存在 executor 冲突、异常或超时风险。

4. `src/robot_switch_server/src/core/adapter_runtime_manager.cpp`
   - `busy_`、`state_`、`running_`、`last_*` 等状态字段部分在锁内读，部分在锁外写。
   - 遥测线程、HTTP 线程、启动/停止路径并发交错时，状态机不满足串行一致性。

5. `src/adapter_go2/src/adapter_go2_node.cpp`
   - `cmd_vel` 仅在收到消息时下发 `Move()` 或 `StopMove()`，没有任何超时自动停机机制。
   - 结果：上位机断流或 topic 停止发布时，代码无法证明机器人一定停下。

#### P2 - Medium

1. `src/robot_adapter_interfaces/src/adapter_client.cpp`
   - 客户端固定依赖 `/safe_stop` 与 `/system_info`。
   - `adapter_go2` 没有注册 `/safe_stop`，`adapter_m20pro` 没有注册 `/system_info`。

2. `src/adapter_go2/src/adapter_go2_node.cpp`
   - `connect()` 中 `StopMove()` 失败仍然视为连接成功。
   - `disconnect()` 在完成 stop/sit 前就先置 `connected_ = false`。
   - 逻辑状态可能领先于实际物理状态。

3. `src/robot_switch_server/src/telemetry/motion_telemetry_reporter.cpp`
   - `system_info` 只在 adapter 类型变化时拉取一次，不构成连续运动状态上报。

#### P3 - Low

1. `README.md`
   - 文档中的 `robot_type`、返回结构、健康检查响应与真实代码不一致。

## 3. 方案选择分析

### 3.1 方案 A：保持适配器为独立 OS 进程

优点：

- 硬件 SDK 与控制面隔离
- 单个 adapter 崩溃不会带崩 HTTP/MQTT/状态机
- 启停后地址空间和 SDK 全局状态可彻底重置
- 对将来多本体扩展最友好

缺点：

- 必须把 spawn / signal / reap / shutdown 设计正确
- 需要额外 supervisor 模块和测试投入

### 3.2 方案 B：改为同进程组件 / 插件

优点：

- 没有 OS 级 zombie / orphan 问题
- 进程内调用路径更短

缺点：

- 一个硬件 SDK 崩溃会拖死整个控制面
- SDK 单例、DDS 资源、后台线程的卸载和复位复杂
- 当前代码的 executor / callback group 问题会更集中、更难排查
- Go2 这类硬件适配更适合进程隔离，而不是强行做进程内插件热插拔

### 3.3 方案 C：混合方案，共享库 + 独立适配器进程

优点：

- 保留 OS 级隔离
- 公共逻辑可以沉淀到库中，减少重复实现
- 适合逐步支持更多 adapter

缺点：

- 初期重构量大于方案 A

### 3.4 最终建议

采用“方案 C”：

- 保留适配器为独立 OS 进程
- 将进程管理抽成独立 supervisor
- 将公共接口和结果类型沉淀到 `robot_adapter_interfaces`
- 控制面、RPC、运动安全分别解耦

原因：

- 这是在“生命周期严格正确”与“硬件 SDK 风险隔离”之间最稳妥的平衡点
- 相比改成同进程插件，该方案更符合本项目现有拓扑和硬件接入现实

## 4. 目标架构

目标架构拆分为 5 个核心层：

1. `robot_switch_server`
   - 仅负责 HTTP、MQTT、统一状态机、监督协调

2. `AdapterProcessSupervisor`
   - 新增模块
   - 专门负责子进程启动、停止、回收、父进程退出清理

3. `AdapterRpcClientHost`
   - 新增模块
   - 使用独立 ROS node + executor 承担所有 adapter 标准服务调用

4. `robot_adapter_interfaces`
   - 保留并增强
   - 提供统一契约、错误模型、状态定义、公共 client 接口

5. `adapter_*`
   - 独立 OS 进程
   - 仅负责具体硬件 SDK 对接和本体控制

## 5. 关键设计决策

### 5.1 进程监督模型

引入 `AdapterProcessSupervisor`，并将其作为唯一进程控制入口。

职责：

- `Spawn(spec)`
- `Terminate(grace_timeout)`
- `Kill(force_timeout)`
- `Shutdown()`
- `PollActive()`
- `TryReapExitedChildren()`

监督信息：

- `pid`
- `pgid`
- `adapter_type`
- `start_time`
- `generation_id`
- `exit_status`
- `is_running`

### 5.2 启动模型

不再让 `AdapterRuntimeManager` 直接执行 `fork/waitpid/kill`。

推荐子进程启动序列：

1. 父进程创建错误回传 pipe
2. `fork()`
3. 子进程 `setpgid(0, 0)`，独立进程组
4. 子进程设置 `prctl(PR_SET_PDEATHSIG, SIGKILL)`
5. 子进程 `execve()`
6. 父进程从 pipe 判断是否 `exec` 成功
7. 父进程将 `pid/pgid/generation_id` 交给 supervisor 持有

不再使用当前的“直接 `setsid()` 后 `execl()`，然后业务层自己记 PID”的模式。

### 5.3 停止模型

统一 stop 序列：

1. `safe_stop`
2. `disconnect`
3. 对 `-pgid` 发送 `SIGTERM`
4. 等待宽限期退出
5. 超时则对 `-pgid` 发送 `SIGKILL`
6. `waitpid` drain 回收

要求：

- 即使业务服务失败，OS 级停止序列也必须继续
- 清理对象必须是整个 adapter 进程组，而不是仅主 PID

### 5.4 崩溃检测与回收模型

不再依赖 `GetStatusWithHealth()` 的缓存刷新顺手回收。

推荐方案：

- 在主进程中统一处理 `SIGCHLD`
- 由 supervisor 内部维护专用 reaper 线程
- 该线程使用 `sigwaitinfo()` 或等价机制等待子进程退出
- 收到事件后循环 `waitpid(-1, WNOHANG)` 直到 drain 完成
- 将“adapter 已退出”的事件投递给 `AdapterRuntimeManager`

收益：

- 避免 zombie
- 与 HTTP 请求、MQTT tick、状态缓存完全解耦
- 崩溃能第一时间转为 `Faulted`

### 5.5 主进程关闭模型

主进程关闭时，必须进入 `ShuttingDown`：

1. 停止接收新 `Start`
2. 停止 HTTP 新请求
3. 对活动 adapter 执行完整 stop 序列
4. 等待 supervisor 确认退出与回收
5. 再关闭 MQTT / ROS

并要求：

- `main()` 正常退出路径触发 `Shutdown()`
- 析构路径触发 `Shutdown()` 兜底

### 5.6 运行时状态机

`AdapterRuntimeManager` 的状态重构为：

- `Idle`
- `Starting`
- `Running`
- `Stopping`
- `Faulted`
- `ShuttingDown`

事件：

- `StartRequested`
- `StartSucceeded`
- `StartFailed`
- `StopRequested`
- `StopSucceeded`
- `StopFailed`
- `ChildExitedUnexpectedly`
- `ShutdownRequested`
- `ShutdownCompleted`

原则：

- 所有事件在单一串行上下文处理
- 不允许再存在“锁内校验 + 锁外写状态”的混合实现
- `cached_snapshot_ / snapshot_valid_` 从当前实现中移除或严格降级为纯展示缓存，不参与 liveness 判定

## 6. RPC 与接口契约重构

### 6.1 新增 `AdapterRpcClientHost`

当前 `AdapterClient` 直接在共享 node 上 `spin_until_future_complete()` 的方式需要替换。

重构方向：

- `AdapterRpcClientHost` 持有独立 ROS node
- `AdapterRpcClientHost` 持有独立 executor / spinning thread
- 所有标准服务调用均经由该模块发起

对上暴露同步接口：

- `Connect(adapter_type)`
- `Disconnect(adapter_type)`
- `SafeStop(adapter_type)`
- `Health(adapter_type)`
- `SystemInfo(adapter_type)`

### 6.2 标准服务契约

所有 adapter 强制实现以下 5 个服务：

- `/adapter_<type>/connect`
- `/adapter_<type>/disconnect`
- `/adapter_<type>/safe_stop`
- `/adapter_<type>/health`
- `/adapter_<type>/system_info`

统一语义：

- `connect`
  - 适配器进入可控状态后才返回 success
- `disconnect`
  - 适配器完成停机与脱离控制后才返回 success
- `safe_stop`
  - 幂等；重复调用仍返回 success
- `health`
  - 表示 adapter 是否可服务
- `system_info`
  - 允许局部字段缺失，但服务本身必须存在

### 6.3 错误模型重构

`AdapterCallResult` 扩展为至少区分：

- 服务不可达
- 服务调用超时
- 服务返回失败
- 服务返回成功

这样 runtime manager 可以更明确地区分：

- adapter 还未就绪
- adapter 已就绪但控制失败
- adapter 已崩溃
- adapter 正在降级

### 6.4 HTTP 契约统一

建议统一对外接口：

- 请求参数统一使用 `adapter_type`
- `/healthz`、`/status`、`/system_info` 的字段名与代码保持一致
- README 与实际响应 JSON 同步
- 如有必要新增 `/capabilities` 接口，暴露支持的 adapter 与服务能力

## 7. Go2 运动控制安全重构

### 7.1 目标

将 `adapter_go2` 从“可发送运动命令”提升为“控制链失效时可自动停机的安全适配器”。

### 7.2 新增 watchdog / deadman

新增参数：

- `cmd_vel_timeout_ms`
- `watchdog_check_interval_ms`
- `stop_on_cmd_vel_timeout`
- `stop_on_shutdown`
- `safe_stop_use_damp`
- `safe_stop_and_sit`

推荐默认值：

- `cmd_vel_timeout_ms = 300~500`
- `watchdog_check_interval_ms = 50~100`
- `stop_on_cmd_vel_timeout = true`
- `stop_on_shutdown = true`

新增状态：

- `Disconnected`
- `ConnectedIdle`
- `ConnectedCommanding`
- `Fault`

新增机制：

- 记录最后一次收到 `cmd_vel` 的时间
- 定时器周期检查是否超时
- 超时后执行 `StopMove()`
- 记录最近一次自动 stop 原因

### 7.3 补齐 `/safe_stop`

Go2 必须补齐标准服务 `/adapter_go2/safe_stop`。

建议语义：

- 默认执行 `StopMove()`
- 如配置启用，可扩展为 `StopMove() + StandDown()`
- 如配置启用高安全模式，可映射为 `Damp()`

同时保留私有接口：

- `/stop`
- `/emergency_stop`
- `/stop_and_sit`

但这些私有接口不再作为 switch_server 统一依赖的契约。

### 7.4 收紧 connect/disconnect 语义

`connect()` 成功条件改为：

- SDK 初始化成功
- 如启用自动站立，则站立成功
- `StopMove()` 成功
- 状态切换为 `ConnectedIdle`

`disconnect()` 改为：

- 执行 `safe_stop`
- 根据配置执行 `StandDown()`
- 全部收敛后再切为 `Disconnected`

禁止：

- 物理 stop 失败但逻辑状态仍视为连接成功
- 在 stop/sit 未完成前先把 `connected_` 置 false

### 7.5 健康与遥测信息增强

`health` / `system_info` 需补充字段：

- 当前控制状态
- watchdog 是否启用
- 最后一次 `cmd_vel` 时间戳
- 最近一次自动 stop 原因

使得上位机和 MQTT 遥测能够区分：

- 真正空闲
- 正在运动
- 已因超时自动停机
- 处于 fault

## 8. 分阶段实施计划

### 阶段 0：基础准备

目标：

- 为大重构建立清晰边界

任务：

1. 冻结当前对外契约，标注哪些字段准备变更
2. 梳理当前 `robot_switch_server`、`robot_adapter_interfaces`、`adapter_go2` 的依赖关系
3. 新增测试专用 `adapter_fake` 包

交付：

- `adapter_fake`
- 基础测试脚手架

### 阶段 1：进程监督器落地

目标：

- 将 OS 级进程管理从 runtime manager 中完全剥离

任务：

1. 新增 `AdapterProcessSupervisor`
2. 引入独立 reaper 线程
3. 实现 `spawn/term/kill/reap/shutdown`
4. 支持进程组级清理
5. 支持 parent-death 兜底

交付：

- 新 supervisor 模块
- 对应单元测试与 fake adapter 集成测试

### 阶段 2：状态机重构

目标：

- 让 `AdapterRuntimeManager` 只负责业务状态转换

任务：

1. 重写状态枚举与事件模型
2. 去掉当前 `snapshot_valid_` 这类会影响 liveness 的缓存逻辑
3. 串行化所有状态写入
4. 接入 supervisor 退出事件

交付：

- 新 runtime manager
- 状态机单元测试

### 阶段 3：RPC Host 重构

目标：

- 去掉共享 node 上的 `spin_until_future_complete()` 调用模型

任务：

1. 新增 `AdapterRpcClientHost`
2. 独立 node + executor + spinning thread
3. 统一 `AdapterCallResult`
4. 逐步替换 runtime manager 中的现有 client 调用

交付：

- 新 RPC host
- 旧 `AdapterClient` 的迁移或适配层

### 阶段 4：标准契约统一

目标：

- 消除“客户端假设”和“adapter 实现”之间的偏差

任务：

1. 强制定义 5 个标准服务
2. 补齐 `adapter_go2/safe_stop`
3. 补齐 `adapter_m20pro/system_info`
4. 将未实现能力统一返回结构化 `not implemented`

交付：

- 完整标准服务契约
- 所有现有 adapter 契约达标

### 阶段 5：Go2 运动安全改造

目标：

- 为实机运动控制加入超时收敛能力

任务：

1. 引入 watchdog timer
2. 新增 `cmd_vel_timeout_ms` 等配置
3. 重构 `connect/disconnect/safe_stop`
4. 新增控制状态上报

交付：

- 改造后的 `adapter_go2`
- 运动控制专项测试

### 阶段 6：遥测、HTTP、文档同步

目标：

- 清理所有对外契约不一致问题

任务：

1. 修正 `/status`、`/system_info` 返回结构
2. 统一 `adapter_type`
3. 更新 README、开发指南、配置示例
4. 校正 MQTT 文档与实际 topic / payload

交付：

- 文档同步
- 上位机对接说明

## 9. 测试与验收矩阵

### 9.1 单元测试

覆盖对象：

- `AdapterProcessSupervisor`
- `AdapterRuntimeManager`
- `AdapterRpcClientHost`
- JSON / Telemetry 构造器

关键用例：

- `Start` 成功
- 启动中重复 `Start`
- `Stop` 成功
- adapter 启动后立即退出
- adapter 崩溃后状态转 `Faulted`
- `Shutdown` 期间拒绝新事务
- `SIGTERM` 超时后升级 `SIGKILL`
- `exec` 失败错误可见

### 9.2 集成测试：`adapter_fake`

测试 fake adapter 需支持可配置行为：

- 正常运行
- 启动后立即退出
- `connect` 恒失败
- `disconnect` 卡住
- `safe_stop` 返回失败
- 收到 `SIGTERM` 延迟退出
- fork 一个子进程用于验证进程组清理

### 9.3 生命周期专项验收

必须验证：

1. 正常 `Start -> Stop` 后无 zombie
2. adapter crash 后无 zombie
3. `robot_switch_server` 正常退出后无 orphan
4. `robot_switch_server` 被 `SIGTERM` 后无 orphan
5. adapter 存在子进程时，整组都被回收
6. 连续启停 100 次，状态机与 PID 不泄漏

### 9.4 Go2 运动安全验收

必须验证：

1. 连续 `cmd_vel` 正常运动
2. `cmd_vel` 中断超过 timeout 后自动 `StopMove()`
3. `disconnect` 必停
4. `safe_stop` 幂等
5. `connect` 失败时不误报 connected
6. `StopMove()` 失败时进入明确失败或 fault

## 10. 风险与回滚

### 10.1 主要风险

1. supervisor 与 ROS2 生命周期线程模型交织，初版容易出现停机死锁
2. Go2 SDK 可能存在未文档化的线程/单例行为
3. 标准契约统一后，上位机调用需要同步切换

### 10.2 风险控制措施

1. 先落地 `adapter_fake`，后接真实 Go2
2. 先做 supervisor + runtime manager，再做运动安全
3. 每阶段保持可运行主线，不做“一次性全改完再联调”

### 10.3 回滚策略

建议按阶段提交，每个阶段单独可回退：

- 阶段 1：仅替换 supervisor，不改 adapter 业务语义
- 阶段 2：仅切状态机
- 阶段 3：仅切 RPC host
- 阶段 4：再统一标准契约
- 阶段 5：最后改 Go2 运动安全

避免将“进程监督重写”和“Go2 控制语义重写”放在同一提交中。

## 11. 建议的实施顺序

推荐执行顺序：

1. 创建 `adapter_fake`
2. 落地 `AdapterProcessSupervisor`
3. 重写 `AdapterRuntimeManager`
4. 新增 `AdapterRpcClientHost`
5. 统一 adapter 标准服务契约
6. 改造 `adapter_go2` watchdog 与 `/safe_stop`
7. 修正文档、README、HTTP/MQTT 对外说明

## 12. 最终建议

本项目后续应遵循以下原则：

1. 适配器继续保持独立 OS 进程
2. 进程管理只允许 supervisor 模块接触 `fork/kill/waitpid`
3. 业务状态机只允许 runtime manager 串行维护
4. 所有 adapter 必须满足统一标准服务契约
5. 所有运动类 adapter 必须实现 watchdog / deadman
6. 生命周期正确性优先于功能扩展速度

如果按本设计推进，当前 review 中提出的高优问题可以系统性关闭，而不是逐点打补丁。
