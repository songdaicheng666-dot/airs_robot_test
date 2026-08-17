# Matrice 4T 与 Go2 Orsus 5G 通信测试

这套程序用于验证以下链路：

```text
电脑结构化 JSON -> 阿里云 -> M4T/Orsus 长轮询 -> 设备遥测 -> 阿里云 -> 电脑
```

首版只允许 `PING` 和 `STATUS_QUERY`，没有 FlyTo、起飞、电机启动、Go2 运动或导航权限调用。联调时 M4T 应保持落地且电机关闭，Go2 应保持静止。

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
scp -r communication_test root@120.24.74.70:/opt/m4t-relay-source/
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
sudo /opt/m4t-relay/venv/bin/pip install -r /opt/m4t-relay/communication_test/cloud/requirements.txt
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

`M4T_DEVICE_TOKEN` 和注册表中 `M4T-001.token` 必须是同一枚 Token。注册表中的 Orsus
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

登录妙算 3 后执行原生编译：

```bash
cd /home/dji/m4t-communication-test/native-build/source
cmake -S Payload-SDK-master -B build-m4t-relay -DCMAKE_BUILD_TYPE=Release
cmake --build build-m4t-relay \
  --target dji_sdk_demo_on_manifold3 --clean-first --parallel 4
```

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
M4T cloud relay service started; flight-control commands are disabled
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
- `valid` 只表示 PSDK 主题已成功订阅并读取，不等于定位解可用。
- 使用位置前还必须检查 `gps.fix_state`、卫星数和精度。`fix_state: 0`、卫星数为 0、精度为极大值时，经纬度不得用于导航。
- 某个 PSDK 主题不可用时，对应对象会返回 `valid: false`，并在 `errors` 中说明。

## 8. 自动测试

电脑端执行云端和 Orsus Agent 测试：

```bash
python3 -m pytest \
  communication_test/tests/test_cloud.py \
  communication_test/tests/test_orsus_agent.py -q
```

PSDK C 代码的编译和编译期检查统一由第 5 节的妙算 3 原生 CMake 构建完成。

## 9. 切换 HTTPS

公网 HTTP 链路通过后：

1. 将域名 A 记录指向 `120.24.74.70`。
2. 在 Nginx 配置可信 TLS 证书和 443，80 只做 HTTPS 跳转。
3. 重新生成 Operator/Device Token。
4. 同时修改电脑 `M4T_BASE_URL` 和妙算 `base_url` 为 `https://<域名>`。
5. 保持 libcurl 的证书和主机名校验开启；程序中没有跳过证书校验的选项。

## 10. Go2 Orsus 接入共享中继

### 10.1 网络关系

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

### 10.2 安装 Agent

Orsus 使用 Ubuntu 22.04 / Python 3.10。当前设备的 APT 数据库中，`edge-core` 存在未满足的
`bluez`/`dnsmasq-base` 依赖，直接执行 `apt install python3-requests` 会失败。不要为此运行
`apt --fix-broken install`，否则可能启动或改变蓝牙、DNS/网络服务。本次使用 Ubuntu 22.04 同版本
的纯 Python 包构建隔离依赖包：

```bash
communication_test/deploy/orsus/build_vendor_archive.sh \
  /tmp/orsus-python-vendor.tar.gz
```

Agent 会优先从 `/opt/orsus-ecs-agent/vendor` 加载 `requests`，不会修改系统 Python 包。安装脚本在
没有传入隔离依赖包且系统无法导入 `requests` 时，才会尝试使用 APT。先根据示例创建私密配置，
填入 ECS 注册表中同一枚 Orsus Token：

```bash
cp communication_test/deploy/orsus/orsus-ecs-agent.env.example \
  communication_test/.private/orsus-ecs-agent.env
chmod 600 communication_test/.private/orsus-ecs-agent.env
```

通过有线管理口上传三个文件。Go2 和 Scout 的有线地址都可能是 `192.168.123.100`，因此使用
独立 `HostKeyAlias`：

```bash
scp -o HostKeyAlias=orsus-go2-wired \
  communication_test/orsus/agent.py \
  communication_test/deploy/orsus/orsus-ecs-agent.service \
  communication_test/.private/orsus-ecs-agent.env \
  /tmp/orsus-python-vendor.tar.gz \
  gs@192.168.123.100:/tmp/

scp -o HostKeyAlias=orsus-go2-wired \
  communication_test/deploy/orsus/install.sh \
  gs@192.168.123.100:/tmp/install-orsus-ecs-agent.sh

ssh -t -o HostKeyAlias=orsus-go2-wired gs@192.168.123.100 \
  'sudo bash /tmp/install-orsus-ecs-agent.sh \
    /tmp/agent.py \
    /tmp/orsus-ecs-agent.env \
    /tmp/orsus-ecs-agent.service \
    /tmp/orsus-python-vendor.tar.gz'
```

服务以 `gs` 用户运行，只读取本机 Edge Core 状态。检查服务和日志：

```bash
ssh -o HostKeyAlias=orsus-go2-wired gs@192.168.123.100 \
  'systemctl status orsus-ecs-agent --no-pager; \
   journalctl -u orsus-ecs-agent -n 100 --no-pager'
```

### 10.3 操作端验证

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

### 10.4 安全边界

- 云端只接受 `PING` 和 `STATUS_QUERY`，Agent 中没有运动或导航写接口。
- 当前公网 HTTP 会明文传输 Bearer Token，只适合本轮无运动联调；Token 泄漏或切换 HTTPS 后必须轮换。
- Agent 日志不得出现 Token；Uvicorn `8000` 继续只监听 ECS 本机，公网仅访问 Nginx `80`。
- 增加任何 Go2 运动、导航或急停命令前，必须先部署 HTTPS 并重新进行现场安全评审。
