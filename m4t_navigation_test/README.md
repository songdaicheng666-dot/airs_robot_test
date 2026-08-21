# M4T 单目标通信导航客户端

该客户端只通过 ECS 与 M4T 妙算 3 relay 通信，不直接连接 PSDK。目标高度是 WGS84
椭球高，不是相对起飞点高度。

```bash
export RELAY_BASE_URL='https://<ecs-domain>'
export RELAY_OPERATOR_TOKEN='<operator-token>'
export RELAY_DEVICE_ID='M4T-001'

python3 -m m4t_navigation_test.client startup
python3 -m m4t_navigation_test.client run
python3 -m m4t_navigation_test.client return-home
```

当 ECS 已明确开启 HTTP 飞行门禁时，外场可用一条命令依次执行无运动 STARTUP 和导航：

```bash
./m4t_navigation_test/field_test.sh
```

脚本不绕过任何端侧门禁；STARTUP 失败时不会提交 NAVIGATE。导航阶段按 Ctrl+C 仍由
现有客户端提交 `CANCEL_NAVIGATION`/RTH。

`run` 当前默认测试目标为纬度 `22.604375789`、经度
`114.057071644`、WGS84 椭球高 `106.0m`。只有显式执行 `run` 才会提交目标；
仍可使用三个坐标选项临时覆盖。妙算端固定最低航路高度为 `2m`、最低目标相对
Home 高度为 `2m`，水平速度为 PSDK 该接口可表达的最小值 `1m/s`。
`2m` 是地面任务的最低航路高度，不会把最终目标强制为相对地面 `2m`。
目标相对高度按“`106.0m` - 地面 STARTUP 时 `POSITION_FUSED.altitude`”计算。

`run` 被 Ctrl+C 中断时，客户端会提交 `CANCEL_NAVIGATION`，由飞机执行 RTH，并等待
落地停桨确认。每个有终态的操作都会在 `m4t_navigation_test/results/` 写入 JSON 和 CSV。
