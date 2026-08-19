# startup_test 机器人启动与自检 demo

该 demo 用于在正式 `navigation_test` 前，通过 ECS 和 5G 建立 Orsus 与 Go2/Scout 的运动控制、
感知扫描和导航容器连接，不依赖 Orsus WebUI：

```text
本机 CLI -> ECS -> Orsus Agent -> Edge Core API
                                  -> motion/start
                                  -> scan/start
                                  -> nav/container/start
                                  -> global relocalization
                                  -> /map_pose_odometry
本机报告 <- ECS <- Orsus Agent <- 状态、当前位置与朝向
```

命令不会发送目标坐标、速度或 mission，但会固定执行全局重定位并返回当前地图中的
`x/y/theta`。重定位阶段可能产生动作，运行前必须清空机器人周围区域。

## 前置部署

真实 ECS 和 Orsus 必须先按 [`navigation_test/README.md`](../navigation_test/README.md) 升级。
设备遥测中的 Agent 版本必须为 `2.4.0` 或更高；低版本不支持 STARTUP 硬门禁与每次导航前重定位，不能运行
本 demo。Go2 固定使用 `go2 / airs1f_3 / navigation / eth3`，Scout 使用设备端独立配置。
Go2 当前的 `nav:runtime-0.2.6` 不接受 `localization` 启动模式；`navigation`
模式仍会固定执行全局重定位。

公网默认要求 HTTPS。临时使用 HTTP 时，ECS 的 `M4T_ALLOW_INSECURE_NAVIGATION=true`、Agent 的
`RELAY_ALLOW_INSECURE_HTTP=true` 和本机 `--allow-insecure-http` 必须同时启用。

## 执行

```bash
export RELAY_BASE_URL=http://120.24.74.70
export RELAY_OPERATOR_TOKEN='<operator-token>'

python3 -m startup_test.client --allow-insecure-http run \
  --device-id ORSUS-GO2-GSM20260003
```

查询已有启动命令：

```bash
python3 -m startup_test.client --allow-insecure-http status '<command-id>'
```

按 Ctrl+C 只停止本机等待，设备端继续完成启动；终端会保留 command ID，可用 `status` 查询。
启动任务与导航任务在同一设备上互斥。启动终态必须为 `COMPLETED`、`result.status=ready`
且 `result.localization.status=successful`，之后再执行 `navigation_test`。后者会验证当前开机周期的
STARTUP 凭据和实时服务状态。只有 STARTUP 已开启的 motion、scan 和导航容器仍健康时，
NAVIGATE 才会强制执行本次全局重定位、返回新鲜位姿，然后提交 mission。STARTUP 位姿用于
确认启动时机器人可在当前地图中正常定位，不作为后续导航的实时起点。

## 报告与测试

报告默认写入 `startup_test/results/`：JSON 保存阶段结果、设备配置、时钟校准和汇总指标，CSV
保存本机-ECS及 Orsus-ECS HTTP RTT 样本。该目录不进入 Git。

```bash
python3 -m pytest communication_test/tests navigation_test/tests startup_test/tests -q
python3 -m unittest discover -s tests -v
```

自动端到端测试使用真实 FastAPI、SQLite、CLI 和 Agent，只模拟 Orsus 控制器，不访问公网或真实
机器人。
