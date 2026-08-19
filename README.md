# Orsus 双机自动重定位导航控制器

该工具通过 Orsus Edge Core 的 HTTP JSON API 控制两台机器人。HTTP 接口使用
`requests.Session`，目标地址为 `http://<Orsus IP>:8898/v1/api`；全局重定位成功后会连接
导航容器在 `7997` 端口提供的 WebSocket，从 `/map_pose_odometry` 读取当前地图位姿。

## 安全边界

- `discover` 和 `preflight` 只读取状态，不会启动设备或移动机器人。
- `startup` 会启动运动适配器、扫描服务和导航容器，并执行全局重定位。全局重定位期间机器人
  可能产生动作，执行前应清空周围区域。
- `run` 会执行 `startup`，随后立即提交配置中的导航任务。
- 示例配置没有地图和任务，不能直接执行 `startup` 或 `run`。
- 不要把一台机器地图中的坐标直接用于另一台机器，除非两张地图已经完成坐标对齐。

## 准备配置

```bash
cp robots.example.yaml robots.yaml
```

编辑 `robots.yaml`，为每台机器人选择真实的 `scene_name`。当前只读探测到的地图如下：

- Go2：`AIRSperi`、`airs1f_3`、`airsback`、`road3`、`scene_isaacsim60_go2_nav2`
- Scout：`airs_inter`、`init_test`、`test0807`

`bringup_mode` 必须与设备安装的导航运行时兼容。当前现场配置中 Go2 使用
`localization`，Scout 的 `runtime-0.2.10` 使用 `navigation`；两种模式后续都会执行相同的
全局重定位流程。

然后为需要执行导航的机器人填写 `mission`。单点绕障导航示例：

```yaml
mission:
  mode: standard
  frame_id: map
  target:
    x: 1.0
    y: 2.0
    theta: 0.0
```

多点路线示例：

```yaml
mission:
  mode: route
  frame_id: map
  cycles: 1
  waypoints:
    - {x: 1.0, y: 2.0, theta: 0.0}
    - {x: 3.0, y: 4.0, theta: 1.57}
```

支持的模式为：

- `standard`：单点绕障导航，需要 `target`。
- `direct`：单点停障导航，需要 `target`。
- `route`：多点路径，需要非空 `waypoints`，可设置 `cycles`。
- `complex`：复合任务，需要 `steps`，步骤类型支持 `navigate`、`fixed_point`、`rotate` 和
  `wait`。

坐标单位为米，`theta` 单位为弧度。当前位置不需要写入配置：程序调用
`/nav/global_relocalization`、验证重定位成功，然后从 `/map_pose_odometry` 返回 `x/y/theta`。

## 使用命令

安装依赖：

```bash
python3 -m pip install -r requirements.txt
```

通过 ECS 和 5G 启动单台机器的 motion/scan/nav 连接并生成自检报告时，使用独立
[`startup_test`](startup_test/README.md)；确认 `COMPLETED/ready` 后，再运行
[`navigation_test`](navigation_test/README.md) 下发正式导航目标。

查看两台设备、适配器、地图和命名点：

```bash
python3 orsus_nav.py --config robots.yaml discover
```

完成只读预检：

```bash
python3 orsus_nav.py --config robots.yaml preflight
```

只启动服务并完成全局重定位，不提交目标：

```bash
python3 orsus_nav.py --config robots.yaml startup
```

启动两台机器人并分别执行配置中的任务：

```bash
python3 orsus_nav.py --config robots.yaml run
```

只选择一台机器人时，把全局参数放在子命令前：

```bash
python3 orsus_nav.py --config robots.yaml --robot scout status
python3 orsus_nav.py --config robots.yaml --robot scout pause
python3 orsus_nav.py --config robots.yaml --robot scout resume
python3 orsus_nav.py --config robots.yaml --robot scout cancel
python3 orsus_nav.py --config robots.yaml --robot scout shutdown
```

`run` 提交任务后会把 `mission_id` 写入 `.orsus_nav_state.json`，因此后续单独运行的
`status` 和 `cancel` 能找到最近任务。也可显式指定任务 ID：

```bash
python3 orsus_nav.py --config robots.yaml --robot scout cancel --mission-id task-001
```

运行 `run` 时按一次 `Ctrl+C`，程序会对所选机器人并行调用
`/nav/stop_navigation`，再取消当前活动 mission，并在退出前短暂确认停止状态。motion
adapter、scan 和导航容器保持运行，因此后续可以直接重新执行导航；完整关闭底层服务仍使用
`shutdown`。

## 实际调用顺序

每台设备独立执行以下流程：

1. 检查 `/healthz`、设备 SN、适配器、地图和 Swagger 路由。
2. 启动 `/services/motion/start`，等待目标适配器为 `CONNECTED`。
3. 启动 `/services/scan/start`，等待定位扫描链路就绪。
4. 使用每台机器配置的 `bringup_mode` 启动 `/nav/container/start`，先等待容器 `running`，
   再等待容器内导航 API 能成功返回状态。
5. 开启 `/nav/relocalization_toggle`。
6. 调用 `/nav/global_relocalization`；默认模式是 `sequential`。
7. 验证 `/nav/navigation_status` 的重定位结果，再从 `7997` WebSocket 读取新鲜地图位姿。
8. 向 `/nav/missions` 提交任务，并轮询 `/nav/missions/{mission_id}`。

提交任务前及任务运行期间，控制器会持续验证运动适配器处于 `CONNECTED`，并检查适配器的
`available` 和 `transport_ready`。运动链路掉线时，该机器的 mission 会被立即取消，另一台机器
继续独立运行。

读取类 GET 请求遇到短暂网络错误时会有限重试。启动、重定位、任务提交、取消等写操作不会自动
重试，避免热点抖动造成重复任务。如果 mission 提交响应丢失，程序会查询聚合导航状态并报告
`submission_unknown`，不会再次提交。

## 测试

```bash
python3 -m unittest discover -s tests -v
```

测试使用模拟 HTTP 客户端，不会访问或移动真实机器人。
