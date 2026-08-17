# Orsus、Go2 与 Scout 调试、部署及故障排查文档

## 0. 文档范围

本文记录 2026-08-09 至 2026-08-10 的 Orsus + Scout 联调流程，按以下顺序组织：

1. 电脑通过网线连接 Orsus，完成 SSH、WiFi 自启动和 GSHub 网页登录。
2. 将新增的 `adapter_scout` 更新到 Orsus，构建并安装为系统运行版本。
3. 排查 Orsus 与 Scout 无法通过 CAN 通信、前端无法开启适配器的问题。
4. 通过 Orsus HTTP API 实现 Go2 和 Scout 的自动重定位与目标点导航。

截至 2026-08-10 的验证结果：

- Orsus 上安装的 `robot-sport` 版本为 `0.1.9-1`。
- 按当前临时方案，Scout USB-CAN 写死为 `can2`、500 Kbit/s，板载接口使用 `can0/can1`、1 Mbit/s。
- 固定硬件命名方案已按要求回退；`can2` 只对当前启动有效，重启后必须重新核对 `gs_usb` 实际编号。
- Scout adapter API 能启动并报告 `CONNECTED`、`can_interface=can2`、`transport_ready=true`。
- Scout 已在 GSHub 网页和 API 导航中验证可正常运动，物理 CAN 链路当前可用。
- Go2 使用 `airs1f_3` 地图，Scout 使用 `airs_inter` 地图；两台机器的 Orsus API 可由同一 Python 程序并发控制。
- Scout 曾因处于地图未知区域而重定位到不可导航位姿，规划器反复执行恢复动作，表现为前后移动；搬到地图已知区域后导航正常。
- 尚未解决的问题是：重启后 USB-CAN 可能不再是 `can2`，导致 Scout 在 GSHub 网页中无法运动。

---

## 1. Orsus 网络与 GSHub 调试

### 1.1 电脑通过网线连接 Orsus

先用网线连接电脑和 Orsus，保证在 WiFi 配置过程中仍有一条稳定的管理链路。电脑必须能够访问 Orsus 当前的管理地址。

本次设备使用的 SSH 地址为：

```text
10.52.104.210
```

先在电脑上确认设备可达：

```bash
ping 10.52.104.210
```

如果 ping 不通，应先检查：

- Orsus 和电脑端网口指示灯是否亮起。
- 电脑是否取得了与 Orsus 管理地址可路由的地址。
- 电脑防火墙、VPN 或虚拟机网络是否改变了到设备网段的路由。
- 设备地址是否已因 DHCP 发生变化。

### 1.2 SSH 登录 Orsus

在电脑终端执行：

```bash
ssh gs@10.52.104.210
```

本次设备的 `gs` 用户密码是一个空格。输入一个空格后按回车即可。

登录后可用以下命令确认设备身份和系统时间：

```bash
hostname
date
uname -a
```

本次设备主机名为 `ms-1826`，系统为 Ubuntu 22.04 / aarch64。

### 1.3 配置 WiFi 并设置开机自动连接

Orsus 使用 NetworkManager 管理 WiFi。本次验证到的无线接口和连接名称为：

```text
WiFi 接口：wlan0
连接名称：sdc
IPv4 获取方式：DHCP（auto）
自动连接：yes
```

先查看网卡与已有连接：

```bash
nmcli device status
nmcli connection show
nmcli device wifi list ifname wlan0
```

如果目标 WiFi 尚未建立连接，可执行：

```bash
sudo nmcli device wifi connect "<WiFi名称>" \
  password "<WiFi密码>" \
  ifname wlan0 \
  name "<连接名称>"
```

如果连接已经存在，只需确保它绑定 `wlan0`、使用 DHCP 并允许自动连接：

```bash
sudo nmcli connection modify "<连接名称>" \
  connection.interface-name wlan0 \
  connection.autoconnect yes \
  ipv4.method auto
```

需要让该 WiFi 优先自动连接时，可设置优先级：

```bash
sudo nmcli connection modify "<连接名称>" \
  connection.autoconnect-priority 100
```

主动拉起连接：

```bash
sudo nmcli connection up "<连接名称>"
```

检查自启动配置是否生效：

```bash
nmcli -g connection.id,connection.interface-name,connection.autoconnect,connection.autoconnect-priority,ipv4.method \
  connection show "<连接名称>"
```

本次设备的检查结果为：

```text
sdc
wlan0
yes
0
auto
```

修改 WiFi 时不要提前断开网线。WiFi 切换可能使无线地址变化或造成当前 SSH 断开，应通过有线链路重新登录后继续检查。

### 1.4 查看 Orsus 在当前 WiFi 中的地址

执行：

```bash
ifconfig wlan0
```

关注 `inet` 字段。本次调试时输出为：

```text
inet 10.52.104.210  netmask 255.255.255.0
```

也可以使用：

```bash
ip -4 address show dev wlan0
```

该地址由 DHCP 分配，换路由器或租约变化后可能改变。后续 SSH 和网页访问都应使用当次查询到的地址，而不是永久假设为 `10.52.104.210`。

### 1.5 登录 GSHub 网页

在与 Orsus 网络互通的电脑浏览器中访问：

```text
http://<Orsus的WiFi地址>:3000
```

本次对应：

```text
http://10.52.104.210:3000
```

当前设备上与机器人控制有关的端口关系如下：

```text
GSHub 网页                  :3000
edge-core 网页后端          :8898
robot_switch_server         :9098
remote_controller WebSocket :9099
```

网页的机器人控制请求先到 `edge-core`，再由它调用 `robot_switch_server`。当前部署中应以 `edge-core.service` 和 8898 API 为准，不应把设备中遗留的旧 `system_controller.service` 当作当前网页控制链路。

---

## 2. `adapter_scout` 更新与部署

### 2.1 Orsus 内部相关文件架构

Orsus 上同时存在“源码工作区”和“系统安装目录”。两者用途不同：

```text
/workspace/robot-sport/
├── ros2_workspace_cpp/                 # C++ ROS 2 源码和打包工作区
│   ├── src/
│   │   ├── robot_adapter_interfaces/   # adapter 公共接口和基础类
│   │   ├── robot_switch_server/        # adapter 启停和 HTTP 控制服务
│   │   ├── remote_controller/          # WebSocket/MQTT 到 cmd_vel 的桥接
│   │   ├── adapter_go2/
│   │   ├── adapter_lynx/
│   │   ├── adapter_fake/
│   │   └── adapter_scout/              # 本次新增的 Scout adapter
│   ├── build/                           # 普通 colcon 构建输出
│   ├── install/                         # 普通 colcon overlay 安装输出
│   ├── log/                             # colcon 日志
│   ├── debian/                          # Debian 包规则和 systemd 文件
│   └── build-deb.sh                     # Debian 包构建入口
└── robot-sport_0.1.9-1_arm64.deb        # 本次生成的安装包

/opt/ros/humble/
├── lib/adapter_scout/adapter_scout_node
├── share/adapter_scout/config/adapter_scout.yaml
└── share/robot_switch_server/config/server.yaml

/usr/lib/robot-sport/launch-robot-sport.sh
/etc/systemd/system/robot-sport.service
/etc/init.d/can_add_server.sh
/opt/gs/edge-core/                       # GSHub 当前网页后端
```

`adapter_scout` 源码包结构为：

```text
ros2_workspace_cpp/src/adapter_scout/
├── CMakeLists.txt
├── package.xml
├── config/adapter_scout.yaml
├── include/adapter_scout/
│   ├── scout_adapter_node.hpp
│   ├── scout_can_client.hpp
│   └── scout_command_codec.hpp
├── src/
│   ├── main.cpp
│   ├── scout_adapter_node.cpp
│   ├── scout_can_client.cpp
│   └── scout_command_codec.cpp
└── test/
    ├── test_scout_can_client.cpp
    └── test_scout_command_codec.cpp
```

各模块职责：

| 模块 | 职责 |
|---|---|
| `ScoutAdapterNode` | 标准 adapter 服务、`cmd_vel` 订阅、健康状态和 500 ms 看门狗 |
| `ScoutCanClient` | 打开、绑定和写入 Linux raw SocketCAN |
| `ScoutCommandCodec` | 限速并生成 Scout 的 `0x421`、`0x111` 控制帧 |
| `robot_switch_server` | 根据 `adapter_type=scout` 启停 `adapter_scout_node` |
| `remote_controller` | 将网页/MQTT 控制转换为 `/<设备SN>/cmd_vel` |

固定命名关系为：

```text
adapter_type=scout
  -> ROS package: adapter_scout
  -> executable:  adapter_scout_node
  -> ROS node:    adapter_scout
  -> service:     /adapter_scout/*
```

### 2.2 更新源码到 Orsus

在本地仓库根目录执行。首次部署或 `adapter_scout` 有完整代码变化时，同步整个 package：

```bash
rsync -av ros2_workspace_cpp/src/adapter_scout/ \
  gs@10.52.104.210:/workspace/robot-sport/ros2_workspace_cpp/src/adapter_scout/
```

同时同步允许 Scout 启动的服务配置和 Debian 版本记录：

```bash
scp ros2_workspace_cpp/src/robot_switch_server/config/server.yaml \
  gs@10.52.104.210:/workspace/robot-sport/ros2_workspace_cpp/src/robot_switch_server/config/server.yaml

scp ros2_workspace_cpp/debian/changelog \
  gs@10.52.104.210:/workspace/robot-sport/ros2_workspace_cpp/debian/changelog
```

`server.yaml` 中必须包含：

```yaml
enabled_adapter_types:
  - "go2"
  - "lynx"
  - "scout"
```

本次最终的 Scout 配置为：

```yaml
adapter_scout:
  ros__parameters:
    can_interface: "can2"
    max_linear_x_mps: 1.5
    max_angular_z_radps: 1.3075
    cmd_vel_timeout_ms: 500
    watchdog_check_interval_ms: 100
```

### 2.3 更新前停止 adapter 并备份

先检查并停止当前 adapter，避免升级过程中遗留子进程：

```bash
curl -sS http://127.0.0.1:9098/status
curl -sS -X POST http://127.0.0.1:9098/stop
```

本次升级前版本为 `0.1.8-1`，备份目录为：

```text
/workspace/backups/robot-sport-pre-can2-20260809-151738
```

推荐至少备份以下内容：

```bash
BACKUP_DIR=/workspace/backups/robot-sport-pre-update-$(date +%Y%m%d-%H%M%S)
mkdir -p "$BACKUP_DIR/system"

cp /workspace/robot-sport/robot-sport_0.1.8-1_arm64.deb "$BACKUP_DIR/"
cp /etc/init.d/can_add_server.sh "$BACKUP_DIR/system/"
cp /opt/ros/humble/share/adapter_scout/config/adapter_scout.yaml \
  "$BACKUP_DIR/system/installed-adapter_scout.yaml"
cp /workspace/robot-sport/ros2_workspace_cpp/src/adapter_scout/config/adapter_scout.yaml \
  "$BACKUP_DIR/system/source-adapter_scout.yaml"
cp /workspace/robot-sport/ros2_workspace_cpp/debian/changelog \
  "$BACKUP_DIR/system/changelog"
```

备份后计算校验值：

```bash
sha256sum "$BACKUP_DIR"/robot-sport_*.deb "$BACKUP_DIR"/system/*
```

### 2.4 在 Orsus 上构建和测试

进入源码工作区：

```bash
cd /workspace/robot-sport/ros2_workspace_cpp
source /opt/ros/humble/setup.bash
```

构建所有源码 package：

```bash
colcon build --base-paths src
```

本次结果为 7 个 package 全部构建成功。

对 Scout 做定向测试：

```bash
colcon test --base-paths src \
  --packages-select adapter_scout \
  --event-handlers console_cohesion+

colcon test-result \
  --test-result-base build/adapter_scout \
  --verbose
```

本次 Scout 测试结果为：

```text
29 tests, 0 errors, 0 failures, 9 skipped
```

全仓测试中的部分 uncrustify/vendor lint 失败属于已有包的格式或第三方代码检查问题，不是本次 Scout CAN 修改引入的功能失败。判断本次部署是否可继续时，应重点看：

- `adapter_scout` 是否编译成功。
- Scout codec 和 CAN client 测试是否通过。
- `robot_switch_server` 是否包含 `scout` 白名单。

### 2.5 构建 Debian 包并安装到系统目录

只执行 `colcon build` 会生成工作区内的 `install/` overlay，但开机服务使用的是 `/opt/ros/humble`。因此正式部署还需要生成并安装 Debian 包：

```bash
cd /workspace/robot-sport/ros2_workspace_cpp
./build-deb.sh
```

本次生成：

```text
/workspace/robot-sport/robot-sport_0.1.9-1_arm64.deb
```

安装前检查包信息：

```bash
dpkg-deb -f /workspace/robot-sport/robot-sport_0.1.9-1_arm64.deb \
  Package Version Architecture Installed-Size
```

应看到：

```text
Package: robot-sport
Version: 0.1.9-1
Architecture: arm64
```

继续确认包内确实包含 Scout 可执行文件和 `can2` 配置：

```bash
dpkg-deb --contents /workspace/robot-sport/robot-sport_0.1.9-1_arm64.deb

dpkg-deb --fsys-tarfile /workspace/robot-sport/robot-sport_0.1.9-1_arm64.deb \
  | tar -xOf - ./opt/ros/humble/share/adapter_scout/config/adapter_scout.yaml
```

确认无误后安装：

```bash
sudo apt install -y \
  /workspace/robot-sport/robot-sport_0.1.9-1_arm64.deb
```

安装脚本会重启 `robot-sport.service`。安装后检查：

```bash
dpkg-query -W -f='${Status} ${Version}\n' robot-sport
systemctl status robot-sport.service --no-pager -l
curl -sS http://127.0.0.1:9098/adapters
curl -sS http://127.0.0.1:9098/status
```

`/adapters` 应包含：

```json
{"enabled_adapter_types":["go2","lynx","scout"]}
```

### 2.6 回滚方法

如果新版本安装后出现不可接受的问题，先停止 adapter，再安装备份包：

```bash
curl -sS -X POST http://127.0.0.1:9098/stop

sudo apt install -y \
  /workspace/backups/robot-sport-pre-can2-20260809-151738/robot-sport_0.1.8-1_arm64.deb
```

需要同时恢复旧 CAN 启动脚本时：

```bash
sudo install -m 0755 \
  /workspace/backups/robot-sport-pre-can2-20260809-151738/system/can_add_server.sh \
  /etc/init.d/can_add_server.sh
```

随后重启服务并检查状态：

```bash
sudo systemctl restart robot-sport.service
curl -sS http://127.0.0.1:9098/status
```

---

## 3. Scout CAN 接口错误的详细排查与解决

### 3.1 初始现象

部署 `adapter_scout` 后，GSHub 页面已经出现 `scout` 选项，但点击开启时按钮无法切换。日志中主要出现三类信息：

```text
adapter instance already running; stop it first
dial tcp 127.0.0.1:7997: connect: connection refused
nav container not running
```

旧版本还出现过：

```text
failed to write CAN frame: No buffer space available
```

这些日志需要分开判断：

| 日志 | 实际含义 | 是否直接证明 CAN 物理断开 |
|---|---|---|
| `adapter instance already running` | `robot_switch_server` 中已有 adapter 子进程，必须先 `/stop` | 否 |
| `127.0.0.1:7997 connection refused` | 导航容器的 odom WebSocket 未运行 | 否 |
| `nav container not running` | 导航服务未启动 | 否 |
| `No buffer space available` | SocketCAN 发送队列无法完成发送，常见于接口选错、位率错误或总线无 ACK | 是，需要检查 CAN |

因此不能仅根据 odom 或导航日志判断 Orsus 与 Scout 是否通信。CAN 必须从接口驱动、位率、收包和错误计数四个方面单独验证。

### 3.2 为什么当前写死 `can2` 只能作为临时方案

`can0`、`can1`、`can2` 是 Linux 为 CAN 网络设备分配的逻辑接口名，不等于外壳上有三个 CAN 插口。

本次外部接线表现为一根 Orsus 电源线和一条 USB-CAN 链路。电源线插好只说明 Orsus 已供电；USB 线插好并在 `lsusb` 中出现，只说明 Orsus 已识别 USB-CAN 适配器。只有适配器的 CAN 侧与 Scout 的 CAN_H、CAN_L、参考地和终端电阻正确连接，并且能够收到 Scout 帧，才能确认完整 CAN 链路已经建立。因此“能插的接口都插上了”不能替代接口归属和收发验证。

Linux 会按照设备枚举顺序分配 `canN`。USB-CAN 曾在不同启动中分别成为 `can1` 和 `can2`，这些编号不代表固定硬件。按当前临时配置，运行关系为：

| 当前接口 | 驱动 | 硬件来源 | 用途 |
|---|---|---|---|
| `can0` | `mttcan` | Orsus 板载 CAN 控制器 1 | 板载保留 |
| `can1` | `mttcan` | Orsus 板载 CAN 控制器 2 | 板载保留 |
| `can2` | `gs_usb` | 外接 USB-CAN | Scout |

该关系已写入当前启动脚本，但没有稳定命名保障；每次重启后都必须用 `ethtool -i can2` 确认其仍为 `gs_usb`，否则不得启动 Scout adapter。

### 3.3 确认 USB-CAN 和接口归属

先查看 USB 设备：

```bash
lsusb -nn
```

本次识别到：

```text
1d50:606f Geschwister Schneider CAN adapter
```

查看所有网络接口：

```bash
ip -br link
```

确认当前硬编码接口的内核驱动和序列号：

```bash
ethtool -i can2
sed -n '1p' /sys/class/net/can2/device/../serial
```

判断规则：

- 路径指向 `mttcan`：板载 CAN。
- 路径指向 `gs_usb`：外接 USB-CAN。

应看到 `driver: gs_usb` 和指定序列号。若 `can2` 变成 `mttcan`，说明重启后编号再次变化，当前硬编码配置不可用。

### 3.4 停止残留 adapter

在修改 CAN 或重复启动前，先查询状态：

```bash
curl -sS http://127.0.0.1:9098/status
```

如果 `active_adapter` 不是 `Unknown`，执行：

```bash
curl -sS -X POST http://127.0.0.1:9098/stop
```

再次查询，确认：

```text
state=DISCONNECTED
active_adapter=Unknown
busy=false
```

这一步解决前端出现 `adapter instance already running; stop it first` 的状态冲突。该错误属于进程生命周期问题，不是 CAN 物理通信问题。

### 3.5 检查并设置正确位率

Scout 旧协议和新 adapter 都要求 500 Kbit/s。当前临时将 `can2` 用于 Scout，板载 `can0/can1` 保持 1 Mbit/s。

先查看当前状态：

```bash
ip -details -statistics link show can2
```

停止 adapter 后，将 USB-CAN 设置为 500 Kbit/s：

```bash
sudo ip link set can2 down
sudo ip link set can2 type can bitrate 500000 restart-ms 100 loopback off
sudo ip link set can2 up
ip -details -statistics link show can2
```

再次检查时应看到：

```text
state ERROR-ACTIVE
bitrate 500000
restart-ms 100
```

`ERROR-ACTIVE` 是 CAN 控制器的正常工作状态，不是报错。真正需要关注的是 `ERROR-PASSIVE`、`BUS-OFF` 以及递增的 `bus-errors`、`error-warn`、`error-pass`、`bus-off` 计数。

板载接口继续保持原有配置：

```text
can0 = 1 Mbit/s
can1 = 1 Mbit/s
```

不要因为 Scout 需要 500 Kbit/s 就把所有 CAN 接口一起修改。

### 3.6 被动监听，确认物理链路确实有数据

接口设置正确后，先不启动 adapter、不发送运动命令，只做被动监听：

```bash
candump -n 20 -e can2,0:0,#FFFFFFFF
```

也可以观察统计计数是否持续增长：

```bash
ip -details -statistics link show can2
```

不能只看 `RX packets` 是否增长。在早期的一次失败诊断中，RX 很快增长到 1,286,475，但开启错误帧显示后确认这些不是有效 Scout 状态帧，而是重复的 CAN 错误帧：

```text
ERRORFRAME
controller-problem{rx-error-passive,tx-error-passive}
no-acknowledgement-on-tx
error-counter-tx-rx{{128}{0}}
```

该次失败当时只能确认：

- USB-CAN 被正确驱动。
- adapter 确实向 `can2` 写入了 CAN 帧。
- 没有其他节点对发送帧进行 ACK，物理总线尚未建立有效通信。

应按顺序检查：

1. Scout 底盘主电源、急停和 CAN 控制模式是否正确。
2. USB-CAN 的 CAN_H/CAN_L 是否连接到 Scout 的正确端子，且没有接反。
3. USB-CAN 与 Scout 是否有参考地。
4. 断电测量 CAN_H 与 CAN_L 之间是否约为 60 欧姆，确认两端 120 欧姆终端电阻。
5. Scout 侧实际位率是否为 500 Kbit/s。
6. 修复后重新被动监听，必须先看到非 `ERRORFRAME` 的有效数据帧，再启动 adapter。

后续重启后，Scout 已在 GSHub 网页遥控和 API 导航中实际运动，说明当前启动中 `can2` 与 Scout 物理链路已经通信。上述错误帧仍作为同类故障的排查样例保留，不再代表最新运行状态。

### 3.7 只发送安全测试帧

只有被动收包确认成功后，才允许发送协议已有的模式帧和零速度帧：

```bash
cansend can2 421#01
cansend can2 111#0000000000000000
```

含义：

- `0x421#01`：进入 Scout CAN 控制模式。
- `0x111#0000000000000000`：线速度和角速度都为 0。

发送后检查：

```bash
ip -details -statistics link show can2
```

早期失败诊断未通过这一前置条件：启动 adapter 后 `can2` 立即进入 `ERROR-PASSIVE` 并报告无 ACK，所以当时没有继续发送手工测试帧或非零速度命令。停止 adapter 后还需要将接口置为 `DOWN`，才能清除持续的硬件重发。

实车接口调试阶段不要使用非零速度作为第一条测试命令。应先完成被动收包、零速发送和错误计数检查，并确保现场具备急停和安全空间。

### 3.8 修改 adapter 配置并持久化开机 CAN 设置

源码配置改为：

```text
/workspace/robot-sport/ros2_workspace_cpp/src/adapter_scout/config/adapter_scout.yaml
```

关键字段：

```yaml
can_interface: "can2"
```

重新构建并安装 `robot-sport_0.1.9-1_arm64.deb` 后，系统运行配置位于：

```text
/opt/ros/humble/share/adapter_scout/config/adapter_scout.yaml
```

当前回退后的配置文件为：

```text
/etc/init.d/can_add_server.sh
/opt/ros/humble/share/adapter_scout/config/adapter_scout.yaml
/workspace/robot-sport/ros2_workspace_cpp/src/adapter_scout/config/adapter_scout.yaml
```

临时启动策略为：

1. 加载 `mttcan` 和 `gs_usb` 等 CAN 内核模块。
2. 将写死的板载 `can0/can1` 设置为 1 Mbit/s。
3. 最多等待 10 秒让写死的 Scout 接口 `can2` 出现。
4. 将 `can2` 设置为 500 Kbit/s、`restart-ms=100` 并拉起。

修改后先做语法检查：

```bash
sh -n /etc/init.d/can_add_server.sh
```

不要在远程联调中直接执行整个 init 脚本，因为它还会修改无线 AP 和其他设备初始化状态。当前方案不解决重启后编号变化；重启后必须先检查 `can2` 的驱动和位率。

### 3.9 分两层验证 adapter 和前端

先直接验证 `robot_switch_server`，排除前端因素：

```bash
curl -sS -X POST \
  'http://127.0.0.1:9098/start?adapter_type=scout'

curl -sS http://127.0.0.1:9098/status
```

本次返回的关键内容为：

```text
state=CONNECTED
active_adapter=scout
robot_type=scout
reachable=true
available=true
can_interface=can2
connected=true
interface_up=true
socket_open=true
transport_ready=true
last_error=""
```

这些字段只证明 adapter 进程成功打开了 SocketCAN 接口，不证明 Scout 底盘已对 CAN 帧进行 ACK。必须同时检查 `ip -details -statistics` 和带错误掩码的 `candump` 输出。

验证后停止：

```bash
curl -sS -X POST http://127.0.0.1:9098/stop
```

再通过 GSHub 实际使用的 `edge-core` API 验证完整前端链路：

```bash
curl -sS -X POST \
  -H 'Content-Type: application/json' \
  -d '{"adapter_type":"scout"}' \
  http://127.0.0.1:8898/v1/api/services/motion/start

curl -sS \
  http://127.0.0.1:8898/v1/api/services/status
```

启动成功后，再从相同链路停止：

```bash
curl -sS -X POST \
  http://127.0.0.1:8898/v1/api/services/motion/stop
```

最终状态应为：

```text
motion.status=stopped
active_adapter=Unknown
state=DISCONNECTED
```

### 3.10 日志验收

查看 `robot-sport` 日志：

```bash
sudo journalctl -u robot-sport.service -n 100 --no-pager
```

adapter 进程成功打开 SocketCAN 时可看到：

```text
adapter_scout ready: SocketCAN=can2
Scout extensions registered: cmd_vel and 500 ms watchdog
Connected to Scout through can2
Disconnected from Scout
```

成功联调后不应出现：

```text
No buffer space available
no-acknowledgement-on-tx
ERROR-PASSIVE
BUS-OFF
transport_ready=false
```

### 3.11 本次问题的根因与处理结论

本次先后遇到了两个独立问题。

第一层是 Linux CAN 接口名不稳定。原配置把某次启动得到的 `can2` 当成永久名称，重启后 USB-CAN 可能变为其他 `canN`：

```text
旧配置：adapter_scout -> can2 -> 依赖枚举顺序
当前临时配置：adapter_scout -> can2 -> 依赖本次枚举结果
```

固定命名修复已按要求回退，因此接口编号问题暂不解决。另一个独立的历史问题是物理 CAN 总线曾没有 ACK：adapter 启动后内核进入 `ERROR-PASSIVE`，`candump` 只收到 `no-acknowledgement-on-tx` 错误帧，没有有效 Scout 状态帧。后续重启后已通过网页遥控和 API 导航验证当前物理链路可用。

同时还有两个容易混淆的伴随问题：

- `adapter instance already running` 是 adapter 残留进程导致的启动冲突，通过 `/stop` 清理。
- odom 7997 连接失败和 `nav container not running` 是导航容器未运行，与 Scout CAN 链路无直接关系。

当前临时处理为：

1. 当前会话中确认 USB-CAN 为 `can2`，驱动为 `gs_usb`。
2. 将 `can2` 设置为 500 Kbit/s，将板载 `can0/can1` 保持为 1 Mbit/s。
3. 将源码和安装版本的 `adapter_scout.yaml` 改为 `can2`。
4. 删除固定命名相关的 udev 规则、systemd 服务和服务依赖。

最新验证中，Scout 能够运动并完成 API 导航。但每次 Orsus 重启后，仍必须先确认 `can2` 的驱动为 `gs_usb`，再进行 Scout 运动或导航测试。

---

## 4. Go2 与 Scout 的 Orsus API 导航控制器

### 4.1 文件和依赖

本地导航控制器位于：

```text
temp_project/
├── orsus_nav.py           # 命令行主程序
├── robots.yaml           # 当前现场配置
├── robots.example.yaml   # 配置模板
├── requirements.txt      # Python 依赖
├── README.md             # 简要用法
├── tests/                # 不连接实车的单元测试
└── .orsus_nav_state.json # 运行后记录 mission_id
```

安装依赖：

```bash
cd ~/Documents/temp_project
python3 -m pip install -r requirements.txt
```

两个外部 Python 包的职责是：

| 依赖 | 用途 |
|---|---|
| `requests>=2.25.1,<3` | 通过 HTTP REST 访问 Orsus `8898` 端口 |
| `PyYAML>=5.4.1,<7` | 读取 `robots.yaml`，不参与网络通信 |

并发调度使用 Python 标准库 `concurrent.futures.ThreadPoolExecutor`。程序没有直接创建 `socket`，也没有使用 WebSocket 或 MQTT；TCP 连接、HTTP 编码和连接池由 `requests`/`urllib3` 内部处理。

### 4.2 程序的编写思路

控制器的边界是“每台 Orsus 是一个独立的 HTTP 服务端”：

```text
orsus_nav.py
  ├── Go2 requests.Session  ──HTTP──> 10.52.104.196:8898
  └── Scout requests.Session──HTTP──> 10.52.104.210:8898
```

主要设计原则如下：

1. 每台机器创建独立的 `requests.Session()`，避免两台设备共享连接状态。
2. Go2 和 Scout 由两个工作线程并发执行，某台超时或失败只记录在该机器结果中，不中断另一台。
3. `robots.yaml` 负责设备地址、SN、adapter、地图、重定位模式和任务目标，业务代码不写死坐标。
4. 在启动服务前先核对健康状态、设备 SN、adapter 白名单、地图和 Swagger 路由，避免把命令发给错误设备。
5. 启动后不只看 HTTP 请求成功，还轮询 motion、scan、container 和导航 API 是否就绪。
6. 每个响应同时检查 HTTP 状态码、顶层 `code` 和 `data.code`，防止 HTTP 200 中包含下游服务错误。
7. 只对状态查询类请求做有限重试；启动、重定位、任务提交和取消不自动重发，避免生成重复任务。
8. 任务提交成功后将 `mission_id` 写入 `.orsus_nav_state.json`，使新启动的 `status`、`cancel` 命令仍能找到该任务。

Go2 和 Scout 的导航运行时版本不同，因此 `bringup_mode` 不能强行设成相同值：

| 机器人 | `adapter_type` | `scene_name` | `bringup_mode` |
|---|---|---|---|
| Go2 | `go2` | `airs1f_3` | `localization` |
| Scout | `scout` | `airs_inter` | `navigation` |

Scout 上的 `runtime-0.2.10` 曾在传入 `localization` 时报错：

```text
Invalid mode. Use mode:=navigation or mode:=mapping.
```

因此 Scout 使用 `navigation`，但后续仍会执行相同的重定位和 mission 流程。

### 4.3 当前 YAML 配置

当前实车配置的核心内容为：

```yaml
robots:
  go2:
    enabled: true
    base_url: http://10.52.104.196:8898
    expected_sn: GSM20260003
    adapter_type: go2
    scene_name: airs1f_3
    bringup_mode: localization
    relocalization:
      mode: sequential
    mission:
      mode: standard
      frame_id: map
      target: {x: -5.303, y: 13.740, theta: -1.5987215948268056}

  scout:
    enabled: true
    base_url: http://10.52.104.210:8898
    expected_sn: GSP20250002
    adapter_type: scout
    scene_name: airs_inter
    bringup_mode: navigation
    relocalization:
      mode: sequential
    mission:
      mode: standard
      frame_id: map
      target: {x: 0.838, y: -2.761, theta: -1.5707963267948966}
```

`x/y` 单位为米，`theta` 单位为弧度，默认坐标系是 `map`。角度换算为：

```text
弧度 = 角度 × pi / 180
```

配置中只写导航目标，不写机器人初始 `x/y/theta`。导航容器启动后，程序调用全局重定位，之后的实时位姿由导航栈维护。

`mission.mode` 支持：

| 模式 | 用途 | 必要字段 |
|---|---|---|
| `standard` | 单点绕障导航 | `target` |
| `direct` | 单点停障导航 | `target` |
| `route` | 多点循环路线 | `waypoints`，可选 `cycles` |
| `complex` | 导航、定点、旋转和等待组合任务 | `steps` |

不同地图之间的 `map` 坐标不能直接通用。未完成坐标对齐时，不得把 Go2 `airs1f_3` 的目标坐标直接用于 Scout `airs_inter`，反之亦然。

### 4.4 重要 API 接口

Orsus Edge Core 基础地址是：

```text
http://<Orsus IP>:8898
```

`/healthz` 和 `/swagger/doc.json` 直接位于基础地址下，业务 API 统一加 `/v1/api` 前缀。导航控制器的核心接口如下：

| 方法 | 路径 | 用途 |
|---|---|---|
| `GET` | `/healthz` | 检查 Edge Core 是否可达 |
| `GET` | `/swagger/doc.json` | 确认当前固件实际暴露的 API |
| `GET` | `/v1/api/systems/device` | 读取设备 SN，防止连错 Orsus |
| `GET` | `/v1/api/services/motion/adapters` | 查看 `go2`/`scout` adapter 是否可用 |
| `GET` | `/v1/api/maps` | 列出已安装地图 |
| `GET` | `/v1/api/nav/landmarks?scene_name=...` | 读取地图命名点 |
| `GET` | `/v1/api/services/status` | 查询 motion、scan 等总体状态 |
| `GET` | `/v1/api/services/motion/status` | 查询 adapter 连接和 CAN transport 健康状态 |
| `POST` | `/v1/api/services/motion/start` | 启动指定机器人 adapter |
| `POST` | `/v1/api/services/motion/stop` | 停止运动 adapter |
| `GET` | `/v1/api/services/scan/status` | 查询感知塔、接收器和 WebSocket 服务 |
| `POST` | `/v1/api/services/scan/start` | 启动定位扫描链路 |
| `POST` | `/v1/api/services/scan/stop` | 停止扫描链路 |
| `GET` | `/v1/api/nav/container/status` | 查询导航容器状态 |
| `POST` | `/v1/api/nav/container/start` | 指定地图和模式启动导航容器 |
| `POST` | `/v1/api/nav/container/stop` | 停止导航容器 |
| `POST` | `/v1/api/nav/get_nav_params` | 核对容器当前地图 |
| `POST` | `/v1/api/nav/relocalization_toggle` | 开启或关闭重定位 |
| `POST` | `/v1/api/nav/global_relocalization` | 执行全局重定位 |
| `POST` | `/v1/api/nav/navigation_status` | 查询导航和重定位状态 |
| `POST` | `/v1/api/nav/missions` | 提交单点、路线或复合任务 |
| `GET` | `/v1/api/nav/missions/{mission_id}` | 轮询任务进度和结果 |
| `DELETE` | `/v1/api/nav/missions/{mission_id}` | 取消指定任务 |
| `POST` | `/v1/api/nav/pause_navigation` | 暂停当前导航 |
| `POST` | `/v1/api/nav/resume_navigation` | 恢复当前导航 |

`/nav/global_relocalization` 没有记载在本次提供的 PDF 版文档中，但两台设备的当前 Swagger 已经暴露该路由。`preflight` 每次都会再次检查；若设备升级后路由消失，程序会报兼容性错误，不会继续导航。

运动 adapter 启动请求示例：

```http
POST /v1/api/services/motion/start
Content-Type: application/json

{"adapter_type":"scout"}
```

导航容器启动请求示例：

```json
{
  "scene_name": "airs_inter",
  "bringup_mode": "navigation",
  "use_relocalization": true,
  "use_sim_time": false,
  "use_online_map": false
}
```

全局重定位请求示例：

```json
{"mode": "sequential"}
```

重定位完成后，`/nav/navigation_status` 的典型关键字段为：

```json
{
  "status": "idle",
  "relocalization": "successful",
  "global_relocalization": "successful"
}
```

单点导航 mission 请求示例：

```json
{
  "mode": "standard",
  "frame_id": "map",
  "target": {
    "x": 0.838,
    "y": -2.761,
    "theta": -1.5707963267948966
  }
}
```

### 4.5 `run` 的完整执行流程

对每台被选中的机器人，程序独立执行：

1. 调用 `/healthz`、`/systems/device`、`/services/motion/adapters`、`/maps` 和 Swagger，核对设备、adapter、地图和全局重定位接口。
2. 启动 motion adapter，轮询到 `status=running`、`state=CONNECTED` 且 `active_adapter` 正确。
3. 启动 scan，等待 `gs-receiver`、`sensors-tower` 和 `websocket-server` 就绪。
4. 使用该机器人的 `scene_name` 和 `bringup_mode` 启动导航容器。
5. 先等待容器报告 `running`，再反复查询 `/nav/navigation_status`，直到容器内部 `7996` 导航 API 可用。
6. 通过 `/nav/get_nav_params` 确认正在使用配置中的地图。如果已有容器使用了其他地图，程序拒绝继续，应先执行 `shutdown`。
7. 调用 `/nav/relocalization_toggle` 启用重定位，再以 `sequential` 模式调用 `/nav/global_relocalization`。
8. 再次查询导航状态和 motion adapter 状态；CAN transport 不健康时不提交 mission。
9. 调用 `/nav/missions` 提交 `robots.yaml` 中的任务，保存返回的 `mission_id`。
10. 每秒查询 `/nav/missions/{mission_id}`，直到 `completed`、`failed` 或 `cancelled`；期间如果 motion adapter 掉线，立即取消该机器人的任务。

导航容器报告 `running` 不等于其内部 API 已经可用。今天曾在过早调用重定位时看到：

```text
POST toggle_relocalization:
dial tcp 172.20.0.5:7996: connect: connection refused
```

现在程序会等待 `/nav/navigation_status` 能正常返回后再继续，已解决该启动时序问题。

### 4.6 常用命令

只读发现，查看两台设备、地图、adapter 和 landmarks：

```bash
python3 orsus_nav.py --config robots.yaml discover
```

只读预检，不启动服务、不移动机器人：

```bash
python3 orsus_nav.py --config robots.yaml preflight
```

只启动 Scout 并执行重定位，不提交目标：

```bash
python3 orsus_nav.py --config robots.yaml --robot scout startup
```

只让 Scout 执行配置中的导航任务：

```bash
python3 orsus_nav.py --config robots.yaml --robot scout run
```

只让 Go2 执行任务：

```bash
python3 orsus_nav.py --config robots.yaml --robot go2 run
```

两台都开机且现场安全时，并发执行：

```bash
python3 orsus_nav.py --config robots.yaml run
```

不带 `--robot` 时，程序会选择 YAML 中所有 `enabled: true` 的机器人。如果 Go2 已关机而只测 Scout，必须使用 `--robot scout`，否则程序仍会并发访问 Go2 并等待网络超时。这不会中断 Scout，但会产生 Go2 超时错误并拖慢命令退出。

状态和任务管理：

```bash
python3 orsus_nav.py --config robots.yaml --robot scout status
python3 orsus_nav.py --config robots.yaml --robot scout pause
python3 orsus_nav.py --config robots.yaml --robot scout resume
python3 orsus_nav.py --config robots.yaml --robot scout cancel
python3 orsus_nav.py --config robots.yaml --robot scout shutdown
```

`--config`、`--robot` 和 `--verbose` 是全局参数，必须写在 `run`、`status` 等子命令之前。`shutdown` 会显式取消任务并依次停止导航容器、scan 和 motion adapter。

运行 `run` 时按一次 `Ctrl+C`，程序会对所选机器人并行调用 `/nav/stop_navigation`，再取消内存或状态文件中记录的活动 mission，并在退出前短暂确认停止状态。该流程不会停止 motion adapter、scan 或导航容器，因此后续可以直接重新执行导航；完整关闭底层服务仍使用 `shutdown`。停止请求使用短超时且彼此隔离，一台机器人异常不会阻止另一台执行停止流程。

### 4.7 Scout 在地图未知区域中前后移动

今天 Scout 导航启动后曾反复前后移动。该现象不是 CAN 接口错误，也不是目标点 `(0.838, -2.761)` 越界。导航日志中的关键信息是：

```text
Robot is out of bounds of the costmap!
Sensor origin at (-0.08, -0.20) is out of map bounds
Start pose is not traversable for AdaptivePlanner
costmap_recovery found no safe local escape
```

`airs_inter` 当时报告的地图范围为：

```text
x: -6.04  到  4.89
y: -11.53 到 -0.31
```

Scout 处于地图中没有有效特征或未被建图的区域，导致起始位姿落在 costmap 外或不可通行区。规划器无法生成正常路径后反复运行 `costmap_recovery`，恢复动作在实车上看起来就是前后乱动。

处理方法已经实车验证：

1. 立即取消 mission，不要让 recovery 持续运动。
2. 手动将 Scout 搬到 `airs_inter` 地图内已知、特征明显且周围有安全空间的区域。
3. 重新执行 `startup` 或 `run`，让 Orsus 重新做全局重定位。
4. 本次搬移后 Scout 能正常规划和运动，确认根因是起始位置的地图可定位性，不是 Python 通信代码。

### 4.8 将已有地图迁移到另一台 Orsus

本节记录 2026-08-11 将本地 `airsback` 地图迁移到 Scout Orsus 的实测过程。目标设备和最终结果为：

| 项目 | 实测值 |
|---|---|
| 本地地图目录 | `/home/sdc/Documents/temp_project/airsback` |
| 目标地址 | `10.52.104.210` |
| 目标主机名 | `ms-1826` |
| 目标型号和 SN | `Orsus-Pro`，`GSP20250002` |
| 主机地图根目录 | `/workspace/edge-static/maps` |
| 导航容器内地图路径 | `/maps/airsback` |
| 原路径元数据备份 | `/workspace/backups/maps/airsback.path.json.GSM20260003.bak` |

迁移完成后，`GET /v1/api/maps` 已识别 `airsback`，报告 4 个文件、总大小
`49,557,676` 字节、分辨率 `0.0500000007`，原点为
`[-57.66510505681907, -69.38194470442897, 0]`。

#### 4.8.1 地图包应包含的文件

本次地图包包含：

```text
airsback/
├── airsback.pcd
├── airsback.pgm
├── airsback.yaml
└── airsback.path.json
```

四个文件的职责不同：

| 文件 | 用途 |
|---|---|
| `airsback.pcd` | 三维点云地图 |
| `airsback.pgm` | 二维占据栅格图 |
| `airsback.yaml` | 栅格分辨率、原点、阈值和 PGM 路径 |
| `airsback.path.json` | 建图路径及其 ROS 坐标帧元数据 |

`airsback.yaml` 中的图像路径是 `/maps/airsback/airsback.pgm`。这里的 `/maps` 是导航
容器内路径，不等于 Orsus 主机上的实际目录。不能只根据 YAML 猜测上传目标；应先从设备上的
现有地图定位主机挂载目录。

#### 4.8.2 上传前只读检查

先验证 SSH 登录和设备身份：

```bash
ssh -o ConnectTimeout=5 gs@10.52.104.210 'hostname; id; date'

ssh gs@10.52.104.210 \
  'curl -fsS http://127.0.0.1:8898/v1/api/systems/device'
```

本次 API 返回的目标设备 SN 是 `GSP20250002`。继续查询现有地图和实际存储目录：

```bash
ssh gs@10.52.104.210 \
  "sudo find /workspace -type f -name 'airs_inter.yaml' 2>/dev/null"

ssh gs@10.52.104.210 \
  'ls -ld /workspace/edge-static/maps; \
   if test -e /workspace/edge-static/maps/airsback; then \
     echo "airsback already exists"; \
   else \
     echo "airsback does not exist"; \
   fi'

ssh gs@10.52.104.210 \
  'df -h /workspace/edge-static/maps'

ssh gs@10.52.104.210 \
  'curl -fsS http://127.0.0.1:8898/v1/api/maps'
```

本次确认主机地图根目录是 `/workspace/edge-static/maps`，目录由 `root:root` 所有，剩余空间
约 296 GB，且目标目录中原先没有 `airsback`。因此采用“普通用户上传到 `/tmp`，校验后由
`sudo` 安装”的流程，不能直接以 `gs` 用户向地图根目录写入。

如果目标设备已经存在同名地图，不要直接覆盖。先确认导航容器没有使用该地图，再把原目录备份到
`/workspace/backups/maps/`，或为新地图使用另一个 `scene_name`。

#### 4.8.3 迁移前处理设备 SN 坐标帧

同型号 Orsus 不代表 ROS 坐标帧可以互换。坐标帧使用每台设备的唯一 SN 作为命名空间。本次
源地图路径文件原本是：

```json
{
  "base_frame": "GSM20260003/base_footprint",
  "frame_id": "GSM20260003/map"
}
```

目标 Scout Orsus 的现有 `airs_inter.path.json` 使用：

```json
{
  "base_frame": "GSP20250002/base_footprint",
  "frame_id": "GSP20250002/map"
}
```

将地图用于目标设备的重定位或导航前，应把 `airsback.path.json` 的这两个字段改为目标 SN，
但不要修改 `path` 数组中的地图坐标。推荐在完整地图首次上传前修改本地文件：

```bash
cd /home/sdc/Documents/temp_project

python3 - GSP20250002 <<'PY'
import json
import sys
from pathlib import Path

device_sn = sys.argv[1]
path = Path("airsback/airsback.path.json")
data = json.loads(path.read_text(encoding="utf-8"))
target_base_frame = f"{device_sn}/base_footprint"
target_frame_id = f"{device_sn}/map"
if (
    data.get("base_frame") != target_base_frame
    or data.get("frame_id") != target_frame_id
):
    data["base_frame"] = target_base_frame
    data["frame_id"] = target_frame_id
    path.write_text(
        json.dumps(data, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
PY

python3 -m json.tool airsback/airsback.path.json >/dev/null
sed -n '1,10p' airsback/airsback.path.json
```

只有在源、目标确实是同一个设备 SN，且运行时 TF 命名空间没有变化时，才应保留原字段。只看
设备型号、机器人底盘类型或“都是 Orsus”不足以作出判断，必须以目标设备
`/v1/api/systems/device` 返回的 `sn` 为准。

#### 4.8.4 上传到临时目录并校验

在本地项目根目录执行。先创建一个明确的临时目录，并防止复用上次残留的数据：

```bash
cd /home/sdc/Documents/temp_project

ssh gs@10.52.104.210 \
  'test ! -e /tmp/airsback-upload && mkdir /tmp/airsback-upload'

rsync -avh --info=progress2 airsback/ \
  gs@10.52.104.210:/tmp/airsback-upload/
```

分别计算本地和远端 SHA-256。两边每个同名文件的哈希必须一致：

```bash
sha256sum \
  airsback/airsback.path.json \
  airsback/airsback.pcd \
  airsback/airsback.pgm \
  airsback/airsback.yaml

ssh gs@10.52.104.210 \
  'sha256sum \
    /tmp/airsback-upload/airsback.path.json \
    /tmp/airsback-upload/airsback.pcd \
    /tmp/airsback-upload/airsback.pgm \
    /tmp/airsback-upload/airsback.yaml'
```

本次最终版本的哈希为：

```text
cd6efa22da2273ac9f531cac7edbf0e6398a4854f9678eb2b81ec74b1b624d57  airsback.path.json
100cb87ea51cd342586edced317bd87c325d09fefebcf8dcb6338b18f39bf394  airsback.pcd
27047d5be1d36e05e2191fb74c86fb2c7043f3f6715505a98526dc8c432e8157  airsback.pgm
4f466db5547602b45015d38636777c90a54b0ea8043d88849df915dda9bdade9  airsback.yaml
```

以上哈希只用于记录本次实际安装的文件。JSON 重新序列化可能只改变空白格式，却产生不同的
`airsback.path.json` 哈希；迁移验收的必要条件是同一次操作中的本地与远端哈希一致，同时
`base_frame`、`frame_id` 和 `path` 数据经过检查，而不是强制匹配这组历史哈希。

#### 4.8.5 原子安装新地图

哈希一致后，在主机地图目录内先复制为隐藏暂存目录，再重命名为正式目录。这样 Edge Core 不会
在文件尚未复制完整时扫描到 `airsback`：

```bash
ssh -t gs@10.52.104.210 "sudo sh -c 'set -eu
  test ! -e /workspace/edge-static/maps/airsback
  test ! -e /workspace/edge-static/maps/.airsback.uploading
  cp -a /tmp/airsback-upload /workspace/edge-static/maps/.airsback.uploading
  chown -R root:root /workspace/edge-static/maps/.airsback.uploading
  chmod -R a+rX /workspace/edge-static/maps/.airsback.uploading
  mv /workspace/edge-static/maps/.airsback.uploading \
    /workspace/edge-static/maps/airsback
'"
```

两个 `test ! -e` 是防误覆盖保护。任一目标已经存在时命令会立即退出，应先检查已有内容，而不是
删除保护或直接加 `--delete`。

#### 4.8.6 已安装后单独修正路径元数据

本次迁移是在完整地图安装后发现源文件仍使用 `GSM20260003`，因此只重新上传并原子替换了
`airsback.path.json`。以后若在首次上传前已按 4.8.3 修改，则不需要重复本步骤。

先上传和校验单个文件：

```bash
scp airsback/airsback.path.json \
  gs@10.52.104.210:/tmp/airsback.path.json.GSP20250002.upload

sha256sum airsback/airsback.path.json

ssh gs@10.52.104.210 \
  'sha256sum /tmp/airsback.path.json.GSP20250002.upload && \
   python3 -m json.tool \
     /tmp/airsback.path.json.GSP20250002.upload >/dev/null'
```

备份原文件，并通过同目录临时文件完成原子替换：

```bash
ssh -t gs@10.52.104.210 "sudo sh -c 'set -eu
  install -d -m 0755 /workspace/backups/maps
  test ! -e /workspace/backups/maps/airsback.path.json.GSM20260003.bak
  cp -a /workspace/edge-static/maps/airsback/airsback.path.json \
    /workspace/backups/maps/airsback.path.json.GSM20260003.bak
  install -o root -g root -m 0664 \
    /tmp/airsback.path.json.GSP20250002.upload \
    /workspace/edge-static/maps/airsback/.airsback.path.json.new
  mv -f /workspace/edge-static/maps/airsback/.airsback.path.json.new \
    /workspace/edge-static/maps/airsback/airsback.path.json
'"
```

本次备份文件哈希为：

```text
4cd161f4970c0b3d8762c976dd03170614ae0cfe48f31b67a8b2140d9c6fa426
```

#### 4.8.7 安装后验证和清理

再次校验正式目录中的文件，并确认 API 已发现地图：

```bash
ssh gs@10.52.104.210 \
  'sha256sum /workspace/edge-static/maps/airsback/*'

curl -fsS http://10.52.104.210:8898/v1/api/maps
```

`/v1/api/maps` 中应出现类似结果：

```json
{
  "name": "airsback",
  "path": "/maps/airsback",
  "size": 49557676,
  "file_count": 4,
  "resolution": 0.0500000007,
  "origin": [-57.66510505681907, -69.38194470442897, 0]
}
```

验证成功后，只删除本次明确创建的临时文件：

```bash
ssh gs@10.52.104.210 'rm -r -- /tmp/airsback-upload'
ssh gs@10.52.104.210 \
  'rm -- /tmp/airsback.path.json.GSP20250002.upload'
```

第二条清理命令只在执行过 4.8.6 的单文件修正流程后需要运行。

上传和调用 `GET /maps` 都不会启动导航容器或移动机器人，也不需要重启 `edge-core`；本次地图在
写入正式目录后已被 API 动态发现。但“API 能列出地图”只证明文件结构和 YAML 元数据可读取，
不等于实车定位、规划和避障已经通过。

正式使用前，把 `robots.yaml` 中 Scout 的 `scene_name` 改为 `airsback`，先执行只读预检：

```bash
python3 orsus_nav.py --config robots.yaml --robot scout preflight
```

确认现场安全、Scout 位于该地图的已知区域且 CAN 链路正常后，再执行 `startup` 做重定位验证。
不要直接执行带目标点的 `run`，也不要沿用 `airs_inter` 或其他地图中的导航坐标。

---

## 5. 未解决：重启后 USB-CAN 不一定仍为 `can2`

### 5.1 一句话总结

Scout adapter 目前仍写死使用 `can2`，但 Linux 重启后会按硬件枚举顺序重新分配 `canN`，因此 USB-CAN 可能变成其他编号，最直观的现象是登录 GSHub 网页后无法控制 Scout 运动。

### 5.2 为什么会发生

`can0`、`can1`、`can2` 是 Linux 的逻辑网络设备名，不是 USB-CAN 的永久硬件身份。当前 Orsus 同时有：

- Jetson 板载 `mttcan` 控制器。
- 外接 `gs_usb` USB-CAN 适配器。

开机时驱动加载、USB 探测和网络设备注册的先后顺序可能变化。本次调试中 USB-CAN 曾被识别为 `can1`，也曾被识别为 `can2`。当前只是把 adapter 和启动脚本临时写死为 `can2`，没有恢复之前回退的 udev/systemd 稳定命名方案。

### 5.3 重启后最直观的检查流程

每次 Scout Orsus 重启后，先不运行 API 导航，执行以下检查：

1. 打开 GSHub：`http://<Scout Orsus IP>:3000`。
2. 选择 Scout adapter 并用网页遥控做小幅度安全测试，现场保持急停可随时操作。
3. 如果网页上 Scout 无法运动，重启后应首先怀疑 USB-CAN 已不再是 `can2`。
4. 在查清 CAN 接口归属前，不要继续执行 `orsus_nav.py ... run`。

网页无法运动不只有接口编号一种原因；急停、底盘电源、CAN_H/CAN_L、位率和终端电阻也可能导致同样现象。但对于“重启前正常，重启后立即不动”的情况，`canN` 编号是第一检查项。

### 5.4 SSH 精确确认方法

登录 Scout Orsus：

```bash
ssh gs@10.52.104.210
```

确认 USB 设备仍存在：

```bash
lsusb -nn
```

应能看到：

```text
1d50:606f Geschwister Schneider CAN adapter
```

查看 CAN 接口和驱动：

```bash
ip -br link
ethtool -i can0
ethtool -i can1
ethtool -i can2
```

关键不是只看 `can2` 是否存在，而是看它的 `driver`：

```text
driver: gs_usb  -> 外接 USB-CAN，应交给 Scout
driver: mttcan  -> Jetson 板载 CAN，不是当前 Scout USB-CAN
```

当前临时配置的正常期望是：

```text
can0 -> mttcan -> 1 Mbit/s
can1 -> mttcan -> 1 Mbit/s
can2 -> gs_usb -> 500 Kbit/s
```

继续检查 USB-CAN 的位率和错误计数：

```bash
ip -details -statistics link show can2
```

正常时应包含 `bitrate 500000`，且不应持续进入 `ERROR-PASSIVE` 或 `BUS-OFF`。再检查 adapter 打开的实际接口：

```bash
curl -sS http://127.0.0.1:9098/status
```

关键字段应为：

```text
active_adapter=scout
can_interface=can2
connected=true
transport_ready=true
```

`CONNECTED` 只证明 adapter 成功打开了该 SocketCAN 接口，不能单独证明 Scout 底盘已经回应。最终仍以 CAN 错误计数、有效数据帧和 GSHub 网页实际运动三者综合验收。

### 5.5 当前临时方案的影响范围

下列文件都依赖 `can2`：

```text
/etc/init.d/can_add_server.sh
/opt/ros/humble/share/adapter_scout/config/adapter_scout.yaml
/workspace/robot-sport/ros2_workspace_cpp/src/adapter_scout/config/adapter_scout.yaml
```

本地仓库中对应的部署副本是：

```text
deployment/scout/can_add_server.sh
deployment/scout/adapter_scout.yaml
```

如果 `gs_usb` 重启后变成了 `can1`，而 adapter 仍打开 `can2`，那么仅修改其中一个文件不能形成完整修复。启动脚本负责位率和接口 `UP`，adapter 配置负责选择 SocketCAN 接口，源码配置负责下一次打包后的默认值，三者必须保持一致。

当前按要求保持写死 `can2`，本轮不继续修复稳定命名。如果重启后网页不能控制 Scout，先记录所有 `canN` 的驱动归属，不要盲目把所有 CAN 都设为 500 Kbit/s，也不要直接开启导航试错。

### 5.6 后续正式修复的建议验收标准

后续再处理此问题时，可在以下两种方案中选择一种：

1. 使用 USB-CAN 序列号或稳定 USB 物理路径建立 udev 持久命名，让 adapter 始终使用固定的逻辑名。
2. 启动时动态遍历 CAN 接口，找到驱动为 `gs_usb` 且序列号匹配的设备，然后统一将该接口传给位率配置和 Scout adapter。

不论采用哪种方案，都不应只验证一次启动。正式验收至少包括：

1. 连续多次完全重启 Orsus，每次都能识别到同一个 USB-CAN。
2. 每次都自动应用 500 Kbit/s，不改变板载 `mttcan` 的 1 Mbit/s 配置。
3. Scout adapter 报告的接口与实际 `gs_usb` 接口一致。
4. CAN 总线无持续 `ERROR-PASSIVE`、`BUS-OFF` 或 `no-acknowledgement-on-tx`。
5. 每次重启后 GSHub 网页都能安全控制 Scout 运动。
6. 网页验证通过后，再执行 `python3 orsus_nav.py --config robots.yaml --robot scout startup` 和一次可取消的短距离导航测试。

---

## 6. Go2 Orsus 通过 5G WiFi 接入 ECS

### 6.1 当前设备和网络

本节设备是安装在 Go2 上的 Orsus-mini，不是 Scout Orsus。Edge Core 返回的身份为：

```text
model=Orsus-mini
sn=GSM20260003
version=v1.0.0
```

电脑通过有线管理口访问它时使用 `192.168.123.100`。由于 Scout Orsus 也可能使用同一个有线
地址，SSH 应使用 `HostKeyAlias=orsus-go2-wired` 单独保存 Go2 密钥，不能把两台设备当成同一主机。

5G 随身 WiFi 在 Orsus 上被识别为 `eth3`：

```text
eth3 = 192.168.0.69/24
```

到公网 ECS 的路由实测为：

```text
120.24.74.70 via 192.168.0.1 dev eth3 src 192.168.0.69
```

其中 `120.24.74.70` 是最终 ECS，`192.168.0.1` 是随身 WiFi 的局域网网关。数据先通过
`eth3` 交给该网关，再由随身 WiFi 经运营商网络转发到公网。这不是把 ECS 地址配置成网关。

### 6.2 通信 Agent

Agent 源码和部署资源位于：

```text
communication_test/orsus/agent.py
communication_test/deploy/orsus/
```

它使用 Python 3.10 和 `requests 2.25.1`，作为 `orsus-ecs-agent.service` 开机启动。当前 Orsus
的 APT 数据库因 `edge-core` 缺少 `bluez`/`dnsmasq-base` 依赖而不能安全安装
`python3-requests`；本次将 Ubuntu 22.04 同版本的纯 Python 依赖隔离安装在
`/opt/orsus-ecs-agent/vendor`，没有执行 `apt --fix-broken install`，也没有修改系统网络服务。

Agent 主动向 ECS 发起 25 秒长轮询并每 5 秒上报状态，因此不需要给 Orsus 配置公网 IP、端口
映射或入站安全组规则。

当前唯一允许的命令为：

```text
PING
STATUS_QUERY
```

周期遥测包含设备身份、Edge Core 健康、motion、scan、导航容器、导航查询、`eth3` 地址和到
ECS 的实际路由。各状态接口独立容错；导航容器停止导致状态查询返回 HTTP 500 时，Agent 仍会
正常上报其他字段。

Agent 不包含 `/services/motion/start`、`/nav/missions`、暂停、恢复、取消或其他运动写接口，
本轮公网 HTTP 验证不会启动或移动 Go2。

### 6.3 服务检查

```bash
ssh -o HostKeyAlias=orsus-go2-wired gs@192.168.123.100 \
  'systemctl status orsus-ecs-agent --no-pager'

ssh -o HostKeyAlias=orsus-go2-wired gs@192.168.123.100 \
  'journalctl -u orsus-ecs-agent -n 100 --no-pager'
```

Agent 的私密配置是 `/etc/orsus-ecs-agent.env`，其中设备 ID 固定为
`ORSUS-GO2-GSM20260003`，设备 Token 必须与 ECS `/etc/m4t-relay-devices.json` 中该设备的
Token 一致。配置文件和日志不得复制到仓库或对外发送。

完整的 ECS 升级、Agent 安装、CLI 验证和回滚步骤见 `communication_test/README.md` 第 10 节。
