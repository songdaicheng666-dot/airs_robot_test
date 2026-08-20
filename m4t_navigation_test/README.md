# M4T 单目标通信导航客户端

该客户端只通过 ECS 与 M4T 妙算 3 relay 通信，不直接连接 PSDK。目标高度是 WGS84
椭球高，不是相对起飞点高度。

```bash
export RELAY_BASE_URL='https://<ecs-domain>'
export RELAY_OPERATOR_TOKEN='<operator-token>'
export RELAY_DEVICE_ID='M4T-001'

python3 -m m4t_navigation_test.client startup
python3 -m m4t_navigation_test.client run \
  --latitude-deg 22.578111 \
  --longitude-deg 113.936960 \
  --altitude-ellipsoid-m 50.0
python3 -m m4t_navigation_test.client return-home
```

`run` 被 Ctrl+C 中断时，客户端会提交 `CANCEL_NAVIGATION`，由飞机执行 RTH，并等待
落地停桨确认。每个有终态的操作都会在 `m4t_navigation_test/results/` 写入 JSON 和 CSV。
