# Matrice 4T 与 Go2 Orsus 5G 通信测试

通过 ECS 启动机器人连接并自检的独立 demo 见 [`startup_test/README.md`](../startup_test/README.md)；
后续下发 Go2/Scout 导航目标、远程取消并生成通信指标报告的流程见
[`navigation_test/README.md`](../navigation_test/README.md)。原有 PING/STATUS_QUERY 联通测试仍可独立使用。
M4T 单目标客户端见 [`m4t_navigation_test/README.md`](../m4t_navigation_test/README.md)。

这套程序用于验证以下链路：

```text
电脑结构化 JSON -> 阿里云 -> M4T/Orsus 长轮询 -> 设备遥测 -> 阿里云 -> 电脑
```

`PING` 和 `STATUS_QUERY` 仍用于无运动联通测试。Go2/Scout 的 `STARTUP` 只通过独立的
`startup_test` 客户端开放，`NAVIGATE`/`CANCEL_NAVIGATION` 只通过 `navigation_test` 开放；
这些写操作均受设备类型、单活动任务、身份和传输安全门禁保护。M4T 已实现独立的
`STARTUP`、单目标 `NAVIGATE`、`CANCEL_NAVIGATION` 和 `RETURN_HOME`，但私密配置默认关闭，
坐标单位未在 DJI Assistant 验证前也会阻断 STARTUP。

## 目录

```text
communication_test/
├── cloud/                  # FastAPI + SQLite 云端中转服务
├── orsus/agent.py          # Go2 Orsus Python 通信 Agent
├── pc/client.py            # 电脑端 JSON 命令客户端
├── tools/device_emulator.py# 不接 PSDK 的妙算模拟器
├── deploy/cloud/           # ECS systemd、Nginx 和设备注册表模板
├── deploy/orsus/           # Orsus systemd、环境变量模板和安装脚本
└── manifold3/              # 历史交叉构建辅助脚本（正式构建不使用）
```

PSDK C 源码位于：

```text
Payload-SDK-master/samples/sample_c/module_sample/m4t_cloud_relay/
```

## 0. 先收紧网络与凭据

阿里云安全组不要保持“所有 TCP 端口”放行。将入方向改为：

- `TCP 22`：仅允许管理电脑当前的公网 IP `/32`。
- `TCP 80`：本次 HTTP 联通测试暂时允许 `0.0.0.0/0`。
- `TCP 443`：配置 HTTPS 时允许 `0.0.0.0/0`。
- 删除其余入站放行规则，不要公开 Uvicorn 的 `8000` 端口。

修改妙算 3 默认登录密码，并配置 SSH 密钥。之前发送过的 DJI App Key 和 License 正式使用前应轮换。

HTTP 阶段的 Bearer Token 会以明文经过公网，因此只用于短时间无飞控测试，切换 HTTPS 后重新生成 Token。

## 1. 部署阿里云中转服务

在当前电脑上上传程序：

```bash
ssh root@120.24.74.70 'mkdir -p /opt/m4t-relay-source'
scp -r communication_test requirements.txt root@120.24.74.70:/opt/m4t-relay-source/
```

登录 ECS 后执行：

```bash
sudo apt update
sudo apt install -y python3-venv nginx openssl
sudo useradd --system --home /opt/m4t-relay --shell /usr/sbin/nologin m4trelay || true
sudo install -d -o root -g root /opt/m4t-relay
sudo install -d -o m4trelay -g m4trelay /var/lib/m4t-relay
sudo cp -a /opt/m4t-relay-source/communication_test /opt/m4t-relay/
sudo python3 -m venv /opt/m4t-relay/venv
sudo /opt/m4t-relay/venv/bin/pip install -r /opt/m4t-relay-source/requirements.txt
```

生成三个不同的 Token：

```bash
openssl rand -hex 32
openssl rand -hex 32
openssl rand -hex 32
```

第一个作为 `M4T_OPERATOR_TOKEN`，第二个作为 `M4T-001` 的设备 Token，第三个作为
`ORSUS-GO2-GSM20260003` 的设备 Token。创建服务私密配置和设备注册表：

```bash
sudo install -m 600 /opt/m4t-relay/communication_test/deploy/cloud/m4t-relay.env.example /etc/m4t-relay.env
sudo install -m 640 -o root -g m4trelay \
  /opt/m4t-relay/communication_test/deploy/cloud/m4t-relay-devices.json.example \
  /etc/m4t-relay-devices.json
sudo nano /etc/m4t-relay.env
sudo nano /etc/m4t-relay-devices.json
```

`M4T_DEVICE_TOKEN` 和注册表中 `M4T-001.token` 必须是同一枚 Token，`M4T-001.expected_sn`
必须是飞机的精确 SN。注册表中的 Orsus
Token 必须与 `/etc/orsus-ecs-agent.env` 一致。替换占位值后安装 systemd 和 Nginx 配置：

```bash
sudo install -m 644 /opt/m4t-relay/communication_test/deploy/cloud/m4t-relay.service /etc/systemd/system/m4t-relay.service
sudo install -m 644 /opt/m4t-relay/communication_test/deploy/cloud/nginx-m4t-relay.conf /etc/nginx/sites-available/m4t-relay
sudo ln -sfn /etc/nginx/sites-available/m4t-relay /etc/nginx/sites-enabled/m4t-relay
sudo unlink /etc/nginx/sites-enabled/default 2>/dev/null || true
sudo nginx -t
sudo systemctl daemon-reload
sudo systemctl enable --now m4t-relay nginx
sudo systemctl status m4t-relay --no-pager
curl http://127.0.0.1/health
curl http://120.24.74.70/health
```

预期健康检查返回：

```json
{"status":"ok"}
```

服务日志：

```bash
sudo journalctl -u m4t-relay -f
```

## 2. 先用模拟器验证公网链路

在电脑终端 A 中设置云端第二个 Token 并启动妙算模拟器：

```bash
export M4T_BASE_URL=http://120.24.74.70
export M4T_DEVICE_ID=M4T-001
export M4T_DEVICE_TOKEN='<云端的 M4T_DEVICE_TOKEN>'
python3 -m communication_test.tools.device_emulator
```

在电脑终端 B 中设置第一个 Token：

```bash
export M4T_BASE_URL=http://120.24.74.70
export M4T_DEVICE_ID=M4T-001
export M4T_OPERATOR_TOKEN='<云端的 M4T_OPERATOR_TOKEN>'
```

发送结构化 JSON：

```bash
python3 -m communication_test.pc.client send \
  --json-file communication_test/examples/ping.json

python3 -m communication_test.pc.client send \
  --json-file communication_test/examples/status_query.json

python3 -m communication_test.pc.client status
```

模拟器返回 `COMPLETED`、`pong` 和 `DEVICE_EMULATOR_NO_PSDK` 就说明“电脑 -> ECS -> 设备端 -> ECS -> 电脑”全链路正常。

## 3. 检查妙算 3 到 ECS 的出站连接

妙算 3 不需要 curl 命令行程序。可以用它现有的 Python 进行健康检查：

```bash
python3 -c "import urllib.request; print(urllib.request.urlopen('http://120.24.74.70/health', timeout=10).read().decode())"
```

必须返回 `{"status":"ok"}` 后再继续 PSDK 联调。

## 4. 准备妙算私密配置

在电脑的项目根目录执行：

```bash
./communication_test/manifold3/prepare_private_files.sh
```

编辑下列两个已被 `.gitignore` 忽略的文件：

```text
Payload-SDK-master/samples/sample_c/platform/linux/manifold3/application/dji_sdk_app_info_private.h
Payload-SDK-master/samples/sample_c/platform/linux/manifold3/app_json/m4t_relay_config.json
```

- App 配置填入轮换后的 App Key、License 和 DJI 开发者账号。
- `USER_APP_ID` 必须与 `app_json/app.json` 中的 `user_app_id` 相同。
- Relay 配置中填入云端的 `M4T_DEVICE_TOKEN`，不要填 Operator Token。
- HTTP 联调时 `base_url` 保持 `http://120.24.74.70`。
- `navigation.expected_aircraft_sn` 必须与 ECS 注册表一致。
- `navigation.state_file_path` 的父目录必须存在、仅 `dji` 可写，并位于持久化存储。
- 初次部署保持 `navigation.enabled=false` 和 `coordinate_units_verified=false`。只有在 DJI Assistant
  模拟器确认 M4T 3.16 单目标接口确实按“度”解释经纬度后，才可在模拟器配置中将两项设为 `true`；
  该配置变更本身是人工验收声明。

## 5. 在妙算 3 上原生编译

正式构建统一在妙算 3 内执行，不部署电脑交叉编译的产物。这样程序直接使用妙算 3 的 GCC、GLIBC、系统头文件和 libcurl。

首次在妙算 3 上准备目录和编译依赖：

```bash
install -d -m 700 /home/dji/m4t-communication-test/native-build/source
sudo apt update
sudo apt install -y cmake gcc g++ make libcurl4-gnutls-dev
timedatectl status
```

应先保证妙算 3 时钟同步。如果设备时间不正确，可在有管理权限时启用 NTP：

```bash
sudo timedatectl set-ntp true
```

在电脑端仅同步源码，不执行编译。`--checksum` 用内容判断变更，避免设备时钟不准导致误判：

```bash
rsync -rc --no-times --chmod=Du=rwx,Dgo=,Fu=rw,Fgo= \
  -e 'ssh -i communication_test/.private/m4t_test_ed25519' \
  Payload-SDK-master/ \
  dji@192.168.0.69:/home/dji/m4t-communication-test/native-build/source/Payload-SDK-master/
```

必须同步完整的 `Payload-SDK-master/`，不得只替换
`m4t_navigation_core.c` 或其他单个文件。妙算上若存在旧的并行构建目录，可能生成不含
飞行状态机的 Relay。构建时输出必须明确包含以下三个对象：

```text
m4t_navigation.c.o
m4t_navigation_core.c.o
m4t_telemetry.c.o
```

登录妙算 3 后执行原生编译：

```bash
cd /home/dji/m4t-communication-test/native-build/source
cmake -S Payload-SDK-master -B build-m4t-relay -DCMAKE_BUILD_TYPE=Release
cmake --build build-m4t-relay \
  --target dji_sdk_demo_on_manifold3 --clean-first --parallel 4

strings build-m4t-relay/bin/dji_sdk_demo_on_manifold3 | \
  grep 'navigation_enabled=%s, coordinate_units_verified=%s'
```

`strings` 检查无输出时不得安装 DPK；这表示构建使用了旧的非飞控 Relay 源码。

妙算 3 上的生成物：

```text
/home/dji/m4t-communication-test/native-build/source/build-m4t-relay/bin/dji_sdk_demo_on_manifold3
/home/dji/m4t-communication-test/native-build/source/build-m4t-relay/dpk/
```

## 6. 先调试可执行文件，再安装 DPK

按大疆文档推荐，先在妙算 3 上直接调试原生产物：

```bash
cd /home/dji/m4t-communication-test/native-build/source
install -d -m 700 ../run
install -d -m 700 /home/dji/m4t-communication-test/state
install -m 700 build-m4t-relay/bin/dji_sdk_demo_on_manifold3 ../run/
install -m 600 \
  Payload-SDK-master/samples/sample_c/platform/linux/manifold3/app_json/m4t_relay_config.json \
  ../run/
cd ../run

# 妙算 3 只能同时运行一个 PSDK 应用。
dji_app_ctl stop Smart3DExplore
./dji_sdk_demo_on_manifold3

# 调试结束后恢复原应用。
dji_app_ctl start Smart3DExplore
```

运行时不应出现 Token 内容，正常日志会包含：

```text
M4T cloud relay service started; navigation_enabled=false, coordinate_units_verified=false
```

此时在电脑端执行：

```bash
python3 -m communication_test.pc.client send \
  --json-file communication_test/examples/status_query.json
python3 -m communication_test.pc.client status
```

确认返回真实 `psdk_connected: true`、飞行状态、经纬高、GPS/RTK 和电池数据后，再通过 DJI Pilot 应用管理安装妙算 3 上 `build-m4t-relay/dpk/` 中的 `.dpk` 包。

## 7. 遥测字段与单位

- `position.latitude_deg/longitude_deg`：十进制度，已从 PSDK 弧度转换。
- `altitude_ellipsoid_m`：WGS84 参考椭球高，不是海拔高。
- `horizontal_accuracy_m/vertical_accuracy_m`：米，已从 PSDK 毫米转换。
- `battery.voltage_v/current_a`：伏特和安培，已从 mV/mA 转换。
- `velocity`：地固 NEU 速度和水平速度，单位 m/s。
- `aircraft`、`session_id`：实际飞控 SN/机型和当前持久化飞行会话。
- `home`、`rth`、`obstacle_avoidance`：Home 基准、返航高度/状态和三向视觉避障开关。
- `safety`、`mission`：本地解锁条件、安全动作、当前命令、阶段和 PSDK 剩余距离/时间。
- `valid` 只表示 PSDK 主题已成功订阅并读取，不等于定位解可用。
- 使用位置前还必须检查 `gps.fix_state`、卫星数和精度。`fix_state: 0`、卫星数为 0、精度为极大值时，经纬度不得用于导航。
- 某个 PSDK 主题不可用时，对应对象会返回 `valid: false`，并在 `errors` 中说明。

## 8. M4T 单目标导航

M4T 与 Go2/Scout 不共享目标 schema。M4T 只接受
`latitude_deg/longitude_deg/altitude_ellipsoid_m`，不接受 `theta`。先配置 HTTPS 和 Operator
Token，再依次执行：

```bash
export RELAY_BASE_URL='https://<ecs-domain>'
export RELAY_OPERATOR_TOKEN='<operator-token>'
export RELAY_DEVICE_ID='M4T-001'

python3 -m m4t_navigation_test.client startup
python3 -m m4t_navigation_test.client run
python3 -m m4t_navigation_test.client return-home
```

`run` 当前默认目标为纬度 `22.604375789`、经度 `114.057071644`、
WGS84 椭球高 `106.0m`；三个坐标选项仍可显式覆盖该目标。

每个 NAVIGATE 消费一次 STARTUP，ready 只保持 300 秒。地面任务可由 PSDK 自动起飞，到点后
悬停；`cancel <navigation_command_id>` 触发 RTH，并在落地停桨后完成。到点悬停后使用
`return-home`。飞行任务仍活动时 RETURN_HOME 返回 409，必须使用 CANCEL。

固定门禁为：电量至少 10%、GPS Fix 3/4、至少 12 星、水平/垂直精度不超过 2m/3m、Home
已设置、视觉避障开启、RTH 高度 20-120m、目标距首次 Home 不超过 100m、目标相对原始地面
椭球高 2-30m。最低航路高度 2m、水平速度 1m/s（PSDK 该字段可表达的最小值），
PC 不可覆盖。`2m` 是最低航路高度，不是最终目标相对高度；后者按
“目标椭球高 - 地面 STARTUP 时 `POSITION_FUSED.altitude`”计算。

PSDK `TOPIC_ALTITUDE_OF_HOMEPOINT` 实际是 ICAO/气压高度，不是 WGS84 椭球高。
为兼容已部署 ECS schema，当前遥测 `home.altitude_ellipsoid_m` 仍临时携带该气压值，
不得用于目标高度计算；导航状态机已改用 `POSITION_FUSED.altitude` 建立地面椭球高基准。

## 9. 自动测试

电脑端执行云端和 Orsus Agent 测试：

```bash
python3 -m pytest \
  communication_test/tests/test_cloud.py \
  communication_test/tests/test_orsus_agent.py \
  communication_test/tests/test_m4t_navigation_core.py \
  communication_test/tests/test_m4t_navigation_adapter.py \
  m4t_navigation_test/tests -q
```

PSDK C 代码的编译和编译期检查统一由第 5 节的妙算 3 原生 CMake 构建完成。

## 10. 切换 HTTPS

公网 HTTP 链路通过后：

1. 将域名 A 记录指向 `120.24.74.70`。
2. 在 Nginx 配置可信 TLS 证书和 443，80 只做 HTTPS 跳转。
3. 重新生成 Operator/Device Token。
4. 同时修改电脑 `M4T_BASE_URL` 和妙算 `base_url` 为 `https://<域名>`。
5. 保持 libcurl 的证书和主机名校验开启；程序中没有跳过证书校验的选项。

## 11. Go2 Orsus 接入共享中继

### 11.1 网络关系

Go2 Orsus 的 5G 网卡实测为：

```text
eth3: 192.168.0.69/24
```

到 ECS 的完整路由为：

```text
120.24.74.70 via 192.168.0.1 dev eth3 src 192.168.0.69
```

`120.24.74.70` 是最终公网 ECS；`192.168.0.1` 是 5G 随身 WiFi 在局域网中的下一跳网关。
随身 WiFi 再通过运营商网络和 NAT 将数据送往 ECS。Orsus 只需具备出站访问能力，ECS 不需要也
无法通过 `192.168.0.69` 主动连接位于私网中的 Orsus。

### 11.2 安装 Agent

Orsus 使用 Ubuntu 22.04 / Python 3.10。当前设备的 APT 数据库中，`edge-core` 存在未满足的
`bluez`/`dnsmasq-base` 依赖，直接执行 `apt install python3-requests` 会失败。不要为此运行
`apt --fix-broken install`，否则可能启动或改变蓝牙、DNS/网络服务。本次使用 Ubuntu 22.04 同版本
的纯 Python 包构建隔离依赖包：

```bash
communication_test/deploy/orsus/build_vendor_archive.sh \
  /tmp/orsus-python-vendor.tar.gz
```

Agent 会优先从 `/opt/orsus-ecs-agent/vendor` 加载 `requests`、`PyYAML` 和 `websocket-client`，不会修改系统 Python 包。安装脚本在
没有传入隔离依赖包且系统无法导入这些包时，才会尝试使用 APT。先根据示例创建私密配置，
填入 ECS 注册表中同一枚 Orsus Token：

```bash
cp communication_test/deploy/orsus/orsus-ecs-agent.env.example \
  communication_test/.private/orsus-ecs-agent.env
chmod 600 communication_test/.private/orsus-ecs-agent.env
```

当电脑与 Go2 Orsus 处于同一随身 WiFi 时，可通过热点管理地址上传 Agent、导航控制器和
部署资源，不强制使用网线：

```bash
scp -o HostKeyAlias=orsus-go2-5g \
  communication_test/orsus/agent.py \
  orsus_nav.py \
  communication_test/deploy/orsus/orsus-ecs-agent.service \
  communication_test/.private/orsus-ecs-agent.env \
  /tmp/orsus-python-vendor.tar.gz \
  gs@192.168.0.69:/tmp/

scp -o HostKeyAlias=orsus-go2-5g \
  communication_test/deploy/orsus/install.sh \
  gs@192.168.0.69:/tmp/install-orsus-ecs-agent.sh

ssh -t -o HostKeyAlias=orsus-go2-5g gs@192.168.0.69 \
  'sudo bash /tmp/install-orsus-ecs-agent.sh \
    /tmp/agent.py \
    /tmp/orsus-ecs-agent.env \
    /tmp/orsus-ecs-agent.service \
    /tmp/orsus-python-vendor.tar.gz \
    /tmp/orsus_nav.py'
```

服务以 `gs` 用户运行；状态查询保持只读，只有通过安全门禁的 `STARTUP`、`NAVIGATE` 和
`CANCEL_NAVIGATION` 才会调用 Edge Core 写接口。检查服务和日志：

```bash
ssh -o HostKeyAlias=orsus-go2-5g gs@192.168.0.69 \
  'systemctl status orsus-ecs-agent --no-pager; \
   journalctl -u orsus-ecs-agent -n 100 --no-pager'
```

### 11.3 操作端验证

使用 ECS 的 Operator Token，不要使用任何设备 Token：

```bash
export RELAY_BASE_URL=http://120.24.74.70
export RELAY_OPERATOR_TOKEN='<M4T_OPERATOR_TOKEN>'
export RELAY_DEVICE_ID=ORSUS-GO2-GSM20260003

python3 -m communication_test.pc.client devices
python3 -m communication_test.pc.client send --type PING --payload '{"message":"hello go2"}'
python3 -m communication_test.pc.client send --type STATUS_QUERY
python3 -m communication_test.pc.client status
```

状态中应包含 SN `GSM20260003`、`eth3` 地址、到 ECS 的路由以及 motion/scan/nav 状态。导航容器
未运行时 `/nav/navigation_status` 返回 HTTP 500 是允许的：Agent 会把它放入 `errors`，设备仍保持在线。

### 11.4 安全边界

- M4T 与 Orsus 都支持 STARTUP/NAVIGATE/CANCEL，但目标 schema 和设备执行器完全分离；仅 M4T
  支持 `RETURN_HOME`。M4T 飞控写操作仍受默认关闭和模拟器坐标单位门槛保护。
- Orsus 必须先完成 `STARTUP/ready` 才能接受 `NAVIGATE`；Agent 会同时校验当前开机周期的启动凭据和实时服务状态。
- 每条 `NAVIGATE` 都会在提交 mission 前重新执行全局重定位并获取新鲜位姿；失败或缺少位姿时不提交 mission。
- 公网 HTTP 会明文传输 Bearer Token 和导航目标。只有 ECS、Agent 和本机三端同时显式启用不安全开关时才允许测试，结束后必须轮换 Token。
- Agent 日志不得出现 Token；Uvicorn `8000` 继续只监听 ECS 本机，公网仅访问 Nginx `80`。
- 正式使用必须部署 HTTPS；启动自检见 `startup_test/README.md`，完整导航部署、取消、指标和实机验收见 `navigation_test/README.md`。
