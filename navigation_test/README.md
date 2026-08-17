# navigation_test 全链路导航测试

该工具验证以下闭环，并把通信时延与导航阶段耗时分开记录：

```text
本机 CLI -> ECS relay -> Orsus Agent -> Edge Core navigation
本机报告 <- ECS relay <- Orsus Agent <- mission completed/failed/cancelled
```

第一阶段支持 Go2 和 Scout 的单点 `standard` 导航，每次只选择一台设备。地图及适配器固定在
设备 Agent 配置中，本机只发送 `x/y/theta`。Go2 当前配置为
`airs1f_3/localization/go2`；Scout 为 `airs_inter/navigation/scout`。

## 1. 本机依赖与测试

本仓库不使用项目虚拟环境。使用当前用户平时使用的系统 `pip3` 安装依赖；在本机无全局写权限时，
pip 会自动写入 `~/.local/lib/python3.10/site-packages`：

```bash
pip3 install -r requirements.txt
python3 -m pytest communication_test/tests navigation_test/tests -q
python3 -m unittest discover -s tests -v
```

## 2. ECS 升级

升级前备份服务配置、注册表和 SQLite 数据库：

```bash
sudo systemctl stop m4t-relay
sudo cp -a /etc/m4t-relay.env /etc/m4t-relay.env.before-navigation
sudo cp -a /etc/m4t-relay-devices.json /etc/m4t-relay-devices.json.before-navigation
sudo cp -a /var/lib/m4t-relay/relay.db /var/lib/m4t-relay/relay.db.before-navigation
```

同步新版 `communication_test` 后沿用现有部署脚本。服务首次启动会给 `commands` 表增量增加进度和
时间戳列，不会重建或清空数据库。设备注册表应包含两个独立 Orsus Token：

```text
ORSUS-GO2-GSM20260003   / GSM20260003
ORSUS-SCOUT-GSP20250002 / GSP20250002
```

默认必须使用 HTTPS。若本轮明确接受公网 HTTP 风险，在 `/etc/m4t-relay.env` 中加入：

```bash
M4T_ALLOW_INSECURE_NAVIGATION=true
```

然后启动并检查：

```bash
sudo systemctl start m4t-relay
sudo systemctl status m4t-relay --no-pager
curl -fsS http://127.0.0.1:8000/health
sudo journalctl -u m4t-relay -n 100 --no-pager
```

回滚时停止服务，恢复三个 `.before-navigation` 文件和上一版程序，再启动服务。新版新增列不会影响
旧版查询，但恢复数据库可获得完全一致的旧状态。

## 3. Go2 Orsus Agent 升级

构建包含 `requests` 和 `PyYAML` 的隔离依赖包，并准备私密环境文件：

```bash
communication_test/deploy/orsus/build_vendor_archive.sh /tmp/orsus-python-vendor.tar.gz
cp communication_test/deploy/orsus/orsus-ecs-agent.env.example \
  communication_test/.private/orsus-go2-agent.env
chmod 600 communication_test/.private/orsus-go2-agent.env
```

填写与 ECS 注册表一致的 Token。HTTPS 未就绪且确需本轮 HTTP 测试时，还必须在 Agent 环境中设置：

```bash
RELAY_ALLOW_INSECURE_HTTP=true
```

上传并安装五个输入文件：

```bash
scp -o HostKeyAlias=orsus-go2-wired \
  communication_test/orsus/agent.py \
  orsus_nav.py \
  communication_test/deploy/orsus/orsus-ecs-agent.service \
  communication_test/.private/orsus-go2-agent.env \
  /tmp/orsus-python-vendor.tar.gz \
  gs@192.168.123.100:/tmp/

scp -o HostKeyAlias=orsus-go2-wired communication_test/deploy/orsus/install.sh \
  gs@192.168.123.100:/tmp/install-orsus-ecs-agent.sh

ssh -t -o HostKeyAlias=orsus-go2-wired gs@192.168.123.100 \
  'sudo bash /tmp/install-orsus-ecs-agent.sh \
    /tmp/agent.py /tmp/orsus-go2-agent.env /tmp/orsus-ecs-agent.service \
    /tmp/orsus-python-vendor.tar.gz /tmp/orsus_nav.py'
```

systemd 使用 `/var/lib/orsus-ecs-agent` 保存 command/mission 恢复状态。导航前确认 ECS 路由确实
经过 `eth3`：

```bash
ssh -o HostKeyAlias=orsus-go2-wired gs@192.168.123.100 \
  'ip -j -4 address show dev eth3; ip -j -4 route get 120.24.74.70; \
   systemctl status orsus-ecs-agent --no-pager'
```

Scout 使用 `orsus-ecs-agent-scout.env.example`。在其 5G 出站链路就绪前，必须把
`ORSUS_NETWORK_INTERFACE=CHANGE_ME` 改为实测接口；否则 Agent 会拒绝导航。

## 4. 本机执行

设置 Operator Token。公网 HTTP 测试必须显式添加 `--allow-insecure-http`；HTTPS 不需要：

```bash
export RELAY_BASE_URL=http://120.24.74.70
export RELAY_OPERATOR_TOKEN='<operator-token>'

python3 -m navigation_test.client --allow-insecure-http run \
  --device-id ORSUS-GO2-GSM20260003 \
  --x -5.303 --y 13.740 --theta -1.5987215948268056
```

查询或取消已有任务：

```bash
python3 -m navigation_test.client --allow-insecure-http status '<command-id>'
python3 -m navigation_test.client --allow-insecure-http cancel \
  --device-id ORSUS-GO2-GSM20260003 '<navigation-command-id>'
```

等待期间按一次 Ctrl+C 会通过 ECS 下发取消，依次调用 `stop_navigation` 和 mission cancel，并等待
停止确认。不要连续按 Ctrl+C 绕过确认。

## 5. 报告与验收

默认在 `navigation_test/results/` 生成：

- JSON：完整命令、导航结果、时钟校准、导航阶段耗时和 RTT 汇总。
- CSV：全部本机-ECS请求以及 Agent 终态携带的最近 128 条 Orsus-ECS HTTP 请求原始
  RTT/成功状态；更长任务在 JSON 中保留完整窗口统计并标明截断数量。

报告包含 `min/avg/p50/p95/p99/max`、命令下行、完成上行和 ECS 到本机通知时延。单向时延使用
本机/Orsus 相对 ECS 的时钟偏移修正，并附带校准不确定度；不与机器人行驶时间混算。

Go2 实机首次验收只使用清场后的短距离目标，并确认：设备在线、路由为 `eth3`、全局重定位成功、
mission 为 `completed`、本机收到 `COMPLETED` 且 JSON 中 `passed=true`。Scout 实机验收等其 5G
接口接入后执行同一流程。

自动端到端测试中的 ECS 和 SQLite 使用真实应用代码，Orsus 导航响应使用模拟控制器，因此不会
访问公网或让机器人运动；真实 5G 时延、定位和物理到达仍只能由上述实机验收确认。
