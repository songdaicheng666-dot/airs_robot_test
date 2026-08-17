# Remote Controller 配置文档

## 目录

- [配置系统概述](#配置系统概述)
- [配置优先级](#配置优先级)
- [配置文件](#配置文件)
- [环境变量](#环境变量)
- [配置验证](#配置验证)
- [使用示例](#使用示例)
- [配置模板](#配置模板)
- [故障排除](#故障排除)
- [最佳实践](#最佳实践)

## 配置系统概述

Remote Controller 采用多层配置系统，支持灵活的配置管理，适用于不同的部署环境。配置系统设计遵循以下原则：

- **分层覆盖**: 支持多个配置源，高优先级配置会覆盖低优先级配置
- **环境适配**: 针对开发、测试、生产环境提供不同的配置策略
- **动态配置**: 支持通过环境变量动态调整关键参数
- **验证机制**: 自动验证配置的有效性和合理性
- **向后兼容**: 保持配置格式的向后兼容性

## 配置优先级

配置系统按照以下优先级顺序加载和应用配置（从高到低）：

```
1. 环境变量 (最高优先级)
   ↓
2. JSON 配置文件
   ↓  
3. 默认值 (最低优先级)
```

### 重要说明

**环境变量支持限制**: 并非所有配置项都支持环境变量覆盖。目前只有 ROS 相关配置支持环境变量：

- ✅ 支持环境变量: `HUB_ID`, `TWIST_TOPIC_QUEUE_SIZE`
- ❌ 不支持环境变量: WebSocket 配置 (`port`, `host`, `max_connections`) 和日志配置 (`level`, `enable_websocket_logs`)

对于不支持环境变量的配置项，必须通过配置文件进行修改。

## 配置文件

### 默认配置文件

默认配置文件位置：`config/remote_controller_config.json`

```json
{
  "websocket": {
    "port": 9099,
    "host": "0.0.0.0",
    "max_connections": 10
  },
  "ros": {
    "twist_topic_queue_size": 10,
    "hub_id": "DEFAULT_HUB_ID"
  },
  "logging": {
    "level": "INFO",
    "enable_websocket_logs": true
  }
}
```

### 配置项详解

#### WebSocket 配置 (`websocket`)

| 配置项 | 类型 | 默认值 | 描述 | 环境变量支持 |
|--------|------|--------|------|--------------|
| `port` | integer | 9099 | WebSocket 服务器监听端口 | ❌ |
| `host` | string | "0.0.0.0" | WebSocket 服务器绑定的主机地址 | ❌ |
| `max_connections` | integer | 10 | 最大并发连接数 | ❌ |

**端口选择建议:**
- 开发环境: 9099 (默认)
- 测试环境: 9098
- 生产环境: 8099 或根据网络策略配置

**主机地址说明:**
- `0.0.0.0`: 监听所有网络接口
- `127.0.0.1`: 仅监听本地回环接口
- 具体 IP: 监听指定网络接口

#### ROS 配置 (`ros`)

| 配置项 | 类型 | 默认值 | 描述 | 环境变量支持 |
|--------|------|--------|------|--------------|
| `hub_id` | string | "DEFAULT_HUB_ID" | 机器人标识符，用于话题命名 | ✅ |
| `twist_topic_queue_size` | integer | 10 | ROS 话题队列大小 | ✅ |

**HUB_ID 命名规范:**
- 使用字母、数字和下划线
- 避免使用特殊字符和空格
- 建议格式: `robot_001`, `warehouse_bot`, `mobile_platform`

**队列大小建议:**
- 低延迟场景: 1-5
- 一般场景: 10 (默认)
- 高可靠性场景: 20-50

#### 日志配置 (`logging`)

| 配置项 | 类型 | 默认值 | 描述 | 环境变量支持 |
|--------|------|--------|------|--------------|
| `level` | string | "INFO" | 日志级别 | ❌ |
| `enable_websocket_logs` | boolean | true | 是否启用 WebSocket 连接日志 | ❌ |

**日志级别:**
- `DEBUG`: 详细调试信息
- `INFO`: 一般信息（推荐）
- `WARN`: 警告信息
- `ERROR`: 仅错误信息

### 自定义配置文件

#### 开发环境配置

`config/development_config.json`:
```json
{
  "websocket": {
    "port": 9099,
    "host": "127.0.0.1",
    "max_connections": 5
  },
  "ros": {
    "twist_topic_queue_size": 5,
    "hub_id": "dev_robot"
  },
  "logging": {
    "level": "DEBUG",
    "enable_websocket_logs": true
  }
}
```

#### 生产环境配置

`config/production_config.json`:
```json
{
  "websocket": {
    "port": 8099,
    "host": "0.0.0.0",
    "max_connections": 20
  },
  "ros": {
    "twist_topic_queue_size": 20,
    "hub_id": "production_robot"
  },
  "logging": {
    "level": "WARN",
    "enable_websocket_logs": false
  }
}
```

#### 测试环境配置

`config/testing_config.json`:
```json
{
  "websocket": {
    "port": 9098,
    "host": "127.0.0.1",
    "max_connections": 3
  },
  "ros": {
    "twist_topic_queue_size": 1,
    "hub_id": "test_robot"
  },
  "logging": {
    "level": "DEBUG",
    "enable_websocket_logs": true
  }
}
```

## 环境变量

### 支持的环境变量

目前仅支持 ROS 相关配置的环境变量覆盖：

#### ROS 配置环境变量

```bash
# 机器人标识符
export HUB_ID="my_robot_001"

# ROS 话题队列大小
export TWIST_TOPIC_QUEUE_SIZE=15
```

#### 配置文件路径

```bash
# 指定自定义配置文件
export REMOTE_CONTROLLER_CONFIG="/path/to/custom_config.json"
```

### 环境变量格式要求

| 变量名 | 类型 | 格式要求 | 示例 |
|--------|------|----------|------|
| `HUB_ID` | string | 非空字符串，建议使用字母数字下划线 | `robot_001` |
| `TWIST_TOPIC_QUEUE_SIZE` | integer | 正整数，建议 1-100 | `15` |
| `REMOTE_CONTROLLER_CONFIG` | string | 有效的文件路径 | `/opt/robot/config.json` |

### 环境变量验证

系统会自动验证环境变量的有效性：

1. **类型检查**: 确保数值类型的环境变量为有效数字
2. **范围检查**: 验证数值在合理范围内
3. **格式检查**: 验证字符串格式符合要求
4. **存在性检查**: 对于文件路径，检查文件是否存在

无效的环境变量会被忽略，并使用默认值或配置文件中的值。

## 配置验证

### 启动时验证

节点启动时会执行以下验证步骤：

```
1. 加载默认配置
   ↓
2. 尝试加载 JSON 配置文件
   ↓
3. 应用环境变量覆盖
   ↓
4. 验证最终配置的有效性
   ↓
5. 显示最终配置信息
```

### 验证规则

#### WebSocket 配置验证

```cpp
// 端口范围验证
if (port < 1024 || port > 65535) {
    RCLCPP_WARN(logger, "端口 %d 超出推荐范围 [1024-65535]", port);
}

// 最大连接数验证
if (max_connections < 1 || max_connections > 100) {
    RCLCPP_WARN(logger, "最大连接数 %d 超出合理范围 [1-100]", max_connections);
}
```

#### ROS 配置验证

```cpp
// HUB_ID 验证
if (hub_id.empty()) {
    RCLCPP_ERROR(logger, "HUB_ID 不能为空");
}

// 队列大小验证
if (queue_size < 1 || queue_size > 1000) {
    RCLCPP_WARN(logger, "队列大小 %d 超出合理范围 [1-1000]", queue_size);
}
```

### 验证日志示例

```bash
[INFO] [remote_controller]: 配置验证开始...
[INFO] [remote_controller]: 默认配置已加载
[INFO] [remote_controller]: 配置文件 '/opt/robot/config.json' 加载成功
[INFO] [remote_controller]: 环境变量 HUB_ID='production_robot' 已应用
[INFO] [remote_controller]: 环境变量 TWIST_TOPIC_QUEUE_SIZE='20' 已应用
[INFO] [remote_controller]: 配置验证通过
[INFO] [remote_controller]: 最终配置:
[INFO] [remote_controller]:   WebSocket 端口: 8099
[INFO] [remote_controller]:   WebSocket 主机: 0.0.0.0
[INFO] [remote_controller]:   最大连接数: 20
[INFO] [remote_controller]:   HUB_ID: production_robot
[INFO] [remote_controller]:   话题队列大小: 20
[INFO] [remote_controller]:   日志级别: WARN
```

## 使用示例

### 基本使用

#### 使用默认配置

```bash
# 最简单的启动方式
ros2 run remote_controller remote_controller_node
```

输出示例:
```
[INFO] [remote_controller]: [Config] HUB_ID: DEFAULT_HUB_ID
[INFO] [remote_controller]: [Config] WebSocket Port: 9099
[INFO] [remote_controller]: [Config] WebSocket Host: 0.0.0.0
[INFO] [remote_controller]: [Config] Twist Topic Queue Size: 10
```

#### 使用环境变量覆盖

```bash
# 设置机器人 ID 和队列大小
export HUB_ID="warehouse_robot_001"
export TWIST_TOPIC_QUEUE_SIZE=25

ros2 run remote_controller remote_controller_node
```

输出示例:
```
[INFO] [remote_controller]: [Config] HUB_ID: warehouse_robot_001
[INFO] [remote_controller]: [Config] WebSocket Port: 9099
[INFO] [remote_controller]: [Config] WebSocket Host: 0.0.0.0
[INFO] [remote_controller]: [Config] Twist Topic Queue Size: 25
```

#### 使用自定义配置文件

```bash
# 设置配置文件路径
export REMOTE_CONTROLLER_CONFIG="/opt/robot/production_config.json"

ros2 run remote_controller remote_controller_node
```

### 高级配置场景

#### 多机器人部署

```bash
# 机器人 1
export HUB_ID="robot_001"
export REMOTE_CONTROLLER_CONFIG="/opt/robot/robot1_config.json"
ros2 run remote_controller remote_controller_node &

# 机器人 2
export HUB_ID="robot_002" 
export REMOTE_CONTROLLER_CONFIG="/opt/robot/robot2_config.json"
ros2 run remote_controller remote_controller_node &

# 机器人 3
export HUB_ID="robot_003"
export REMOTE_CONTROLLER_CONFIG="/opt/robot/robot3_config.json"
ros2 run remote_controller remote_controller_node &
```

#### 开发环境快速切换

```bash
# 开发模式
export REMOTE_CONTROLLER_CONFIG="$(pwd)/config/development_config.json"
export HUB_ID="dev_robot"
ros2 run remote_controller remote_controller_node

# 测试模式
export REMOTE_CONTROLLER_CONFIG="$(pwd)/config/testing_config.json"
export HUB_ID="test_robot"
ros2 run remote_controller remote_controller_node
```

#### Docker 容器部署

```dockerfile
# Dockerfile
FROM ros:humble

# 复制配置文件
COPY config/production_config.json /opt/robot/config.json

# 设置环境变量
ENV REMOTE_CONTROLLER_CONFIG=/opt/robot/config.json
ENV HUB_ID=container_robot

# 设置启动命令
CMD ["ros2", "run", "remote_controller", "remote_controller_node"]
```

```bash
# 运行容器时覆盖配置
docker run -e HUB_ID=production_robot_001 \
           -e TWIST_TOPIC_QUEUE_SIZE=30 \
           my_robot_image
```

#### Launch 文件配置

```python
# launch/remote_controller.launch.py
from launch import LaunchDescription
from launch.actions import SetEnvironmentVariable
from launch_ros.actions import Node

def generate_launch_description():
    return LaunchDescription([
        # 设置环境变量
        SetEnvironmentVariable(
            name='HUB_ID',
            value='launch_robot'
        ),
        SetEnvironmentVariable(
            name='TWIST_TOPIC_QUEUE_SIZE',
            value='15'
        ),
        SetEnvironmentVariable(
            name='REMOTE_CONTROLLER_CONFIG',
            value='/path/to/launch_config.json'
        ),
        
        # 启动节点
        Node(
            package='remote_controller',
            executable='remote_controller_node',
            name='remote_controller',
            output='screen',
            parameters=[{
                # 还可以通过 ROS 参数传递配置
                'additional_param': 'value'
            }]
        )
    ])
```

## 配置模板

### 配置文件模板生成器

```bash
#!/bin/bash
# scripts/generate_config.sh

CONFIG_TYPE=${1:-development}
OUTPUT_FILE=${2:-"config/${CONFIG_TYPE}_config.json"}

case $CONFIG_TYPE in
    "development")
        cat > "$OUTPUT_FILE" << EOF
{
  "websocket": {
    "port": 9099,
    "host": "127.0.0.1",
    "max_connections": 5
  },
  "ros": {
    "twist_topic_queue_size": 5,
    "hub_id": "dev_robot"
  },
  "logging": {
    "level": "DEBUG",
    "enable_websocket_logs": true
  }
}
EOF
        ;;
    "production")
        cat > "$OUTPUT_FILE" << EOF
{
  "websocket": {
    "port": 8099,
    "host": "0.0.0.0",
    "max_connections": 20
  },
  "ros": {
    "twist_topic_queue_size": 20,
    "hub_id": "production_robot"
  },
  "logging": {
    "level": "WARN",
    "enable_websocket_logs": false
  }
}
EOF
        ;;
    "testing")
        cat > "$OUTPUT_FILE" << EOF
{
  "websocket": {
    "port": 9098,
    "host": "127.0.0.1",
    "max_connections": 3
  },
  "ros": {
    "twist_topic_queue_size": 1,
    "hub_id": "test_robot"
  },
  "logging": {
    "level": "DEBUG",
    "enable_websocket_logs": true
  }
}
EOF
        ;;
    *)
        echo "未知的配置类型: $CONFIG_TYPE"
        echo "支持的类型: development, production, testing"
        exit 1
        ;;
esac

echo "配置文件已生成: $OUTPUT_FILE"
```

使用示例:
```bash
# 生成开发环境配置
./scripts/generate_config.sh development

# 生成生产环境配置
./scripts/generate_config.sh production /opt/robot/prod_config.json

# 生成测试环境配置
./scripts/generate_config.sh testing
```

### 环境变量模板

#### 开发环境 (`.env.development`)

```bash
# Remote Controller 开发环境配置
export HUB_ID="dev_robot"
export TWIST_TOPIC_QUEUE_SIZE=5
export REMOTE_CONTROLLER_CONFIG="$(pwd)/config/development_config.json"

# 可选: ROS 2 相关环境变量
export ROS_DOMAIN_ID=1
export RMW_IMPLEMENTATION=rmw_cyclonedx_cpp
```

#### 生产环境 (`.env.production`)

```bash
# Remote Controller 生产环境配置
export HUB_ID="production_robot"
export TWIST_TOPIC_QUEUE_SIZE=20
export REMOTE_CONTROLLER_CONFIG="/opt/robot/config/production_config.json"

# 生产环境 ROS 配置
export ROS_DOMAIN_ID=0
export RMW_IMPLEMENTATION=rmw_cyclonedx_cpp
export RCUTILS_COLORIZED_OUTPUT=0
```

#### 测试环境 (`.env.testing`)

```bash
# Remote Controller 测试环境配置
export HUB_ID="test_robot"
export TWIST_TOPIC_QUEUE_SIZE=1
export REMOTE_CONTROLLER_CONFIG="$(pwd)/config/testing_config.json"

# 测试环境 ROS 配置
export ROS_DOMAIN_ID=2
export RMW_IMPLEMENTATION=rmw_cyclonedx_cpp
```

使用方法:
```bash
# 加载开发环境配置
source .env.development
ros2 run remote_controller remote_controller_node

# 加载生产环境配置
source .env.production
ros2 run remote_controller remote_controller_node
```

## 故障排除

### 常见配置问题

#### 1. 配置文件加载失败

**问题**: 节点启动时显示配置文件加载警告

```
[WARN] [remote_controller]: 配置文件加载失败，使用默认配置
```

**可能原因**:
- 配置文件路径错误
- 配置文件不存在
- JSON 语法错误
- 文件权限问题

**解决方案**:

```bash
# 检查文件是否存在
ls -la $REMOTE_CONTROLLER_CONFIG

# 检查 JSON 语法
python3 -m json.tool $REMOTE_CONTROLLER_CONFIG

# 检查文件权限
chmod 644 $REMOTE_CONTROLLER_CONFIG

# 验证文件路径
export REMOTE_CONTROLLER_CONFIG="$(pwd)/config/remote_controller_config.json"
```

#### 2. WebSocket 端口被占用

**问题**: 节点启动失败，提示端口被占用

```
[ERROR] [remote_controller]: WebSocket 服务器启动失败: Address already in use
```

**解决方案**:

```bash
# 检查端口占用情况
netstat -tlnp | grep 9099
# 或者
ss -tlnp | grep 9099

# 查找占用端口的进程
lsof -i :9099

# 终止占用进程（谨慎操作）
sudo kill -9 <PID>

# 或者使用不同的端口（通过配置文件修改）
# 修改配置文件中的 websocket.port 值
```

#### 3. 环境变量不生效

**问题**: 设置了环境变量但配置没有变化

**可能原因**:
- 该配置项不支持环境变量覆盖
- 环境变量格式错误
- 环境变量未正确导出

**解决方案**:

```bash
# 检查环境变量是否已设置
echo $HUB_ID
echo $TWIST_TOPIC_QUEUE_SIZE

# 确保正确导出环境变量
export HUB_ID="my_robot"  # 注意使用 export

# 验证支持的环境变量列表
# 目前仅支持: HUB_ID, TWIST_TOPIC_QUEUE_SIZE

# 对于不支持环境变量的配置，必须通过配置文件修改
# 例如 WebSocket 端口必须在配置文件中修改
```

#### 4. 配置验证失败

**问题**: 配置值超出合理范围

```
[WARN] [remote_controller]: 端口 80 超出推荐范围 [1024-65535]
```

**解决方案**:

```bash
# 检查配置值的合理性
# 端口范围: 1024-65535
# 队列大小: 1-1000
# 最大连接数: 1-100

# 修改配置文件中的对应值
vim config/remote_controller_config.json
```

### 调试配置问题

#### 启用详细日志

```bash
# 使用调试配置文件
export REMOTE_CONTROLLER_CONFIG="$(pwd)/config/development_config.json"

# 或者创建调试专用配置
cat > debug_config.json << EOF
{
  "websocket": {
    "port": 9099,
    "host": "127.0.0.1",
    "max_connections": 5
  },
  "ros": {
    "twist_topic_queue_size": 5,
    "hub_id": "debug_robot"
  },
  "logging": {
    "level": "DEBUG",
    "enable_websocket_logs": true
  }
}
EOF

export REMOTE_CONTROLLER_CONFIG="$(pwd)/debug_config.json"
ros2 run remote_controller remote_controller_node
```

#### 配置检查脚本

```bash
#!/bin/bash
# scripts/check_config.sh

echo "=== Remote Controller 配置检查 ==="

# 检查环境变量
echo "环境变量:"
echo "  HUB_ID: ${HUB_ID:-未设置}"
echo "  TWIST_TOPIC_QUEUE_SIZE: ${TWIST_TOPIC_QUEUE_SIZE:-未设置}"
echo "  REMOTE_CONTROLLER_CONFIG: ${REMOTE_CONTROLLER_CONFIG:-未设置}"

# 检查配置文件
if [ -n "$REMOTE_CONTROLLER_CONFIG" ]; then
    echo
    echo "配置文件检查:"
    if [ -f "$REMOTE_CONTROLLER_CONFIG" ]; then
        echo "  文件存在: ✓"
        echo "  文件权限: $(ls -la $REMOTE_CONTROLLER_CONFIG | cut -d' ' -f1)"
        
        # 检查 JSON 语法
        if python3 -m json.tool "$REMOTE_CONTROLLER_CONFIG" > /dev/null 2>&1; then
            echo "  JSON 语法: ✓"
            
            # 显示配置内容
            echo "  配置内容:"
            python3 -m json.tool "$REMOTE_CONTROLLER_CONFIG" | sed 's/^/    /'
        else
            echo "  JSON 语法: ✗"
        fi
    else
        echo "  文件存在: ✗"
    fi
else
    echo
    echo "使用默认配置文件: config/remote_controller_config.json"
fi

# 检查端口占用
echo
echo "端口检查:"
DEFAULT_PORT=9099
if netstat -tln | grep -q ":$DEFAULT_PORT "; then
    echo "  端口 $DEFAULT_PORT: 已被占用 ✗"
    echo "  占用进程:"
    lsof -i :$DEFAULT_PORT | sed 's/^/    /'
else
    echo "  端口 $DEFAULT_PORT: 可用 ✓"
fi

echo
echo "=== 检查完成 ==="
```

使用方法:
```bash
chmod +x scripts/check_config.sh
./scripts/check_config.sh
```

## 最佳实践

### 1. 环境分离

为不同的部署环境创建专用配置：

```
config/
├── remote_controller_config.json     # 默认配置
├── development_config.json           # 开发环境
├── testing_config.json              # 测试环境
├── staging_config.json              # 预发布环境
└── production_config.json           # 生产环境
```

### 2. 配置管理策略

#### 开发阶段

```bash
# 使用开发配置和环境变量快速调试
export REMOTE_CONTROLLER_CONFIG="$(pwd)/config/development_config.json"
export HUB_ID="dev_$(whoami)"
export TWIST_TOPIC_QUEUE_SIZE=1
```

#### 测试阶段

```bash
# 使用测试配置确保一致性
export REMOTE_CONTROLLER_CONFIG="$(pwd)/config/testing_config.json"
export HUB_ID="test_robot"
```

#### 生产部署

```bash
# 使用生产配置，最小化环境变量使用
export REMOTE_CONTROLLER_CONFIG="/opt/robot/config/production_config.json"
export HUB_ID="robot_$(hostname)"
```

### 3. 安全考虑

#### 文件权限

```bash
# 设置合适的文件权限
chmod 644 config/*.json
chown root:robot config/*.json
```

#### 敏感信息处理

```bash
# 避免在配置文件中存储敏感信息
# 使用环境变量或专用的密钥管理系统

# 错误做法
{
  "auth": {
    "password": "secret123"  // 不要这样做
  }
}

# 正确做法
{
  "auth": {
    "password_env": "ROBOT_PASSWORD"  // 从环境变量读取
  }
}
```

### 4. 版本控制

#### .gitignore 配置

```bash
# .gitignore
config/local_*.json
config/*_local.json
.env.local
*.env.local
```

#### 配置模板管理

```bash
# 提交配置模板，不提交实际配置
config/
├── remote_controller_config.json.template
├── development_config.json.template
└── production_config.json.template

# 部署时复制模板并修改
cp config/production_config.json.template config/production_config.json
# 编辑 production_config.json
```

### 5. 监控和告警

#### 配置检查脚本

```bash
#!/bin/bash
# scripts/config_health_check.sh

# 检查关键配置项
check_config_health() {
    local config_file="$1"
    
    if [ ! -f "$config_file" ]; then
        echo "ERROR: 配置文件不存在: $config_file"
        return 1
    fi
    
    # 检查端口是否在合理范围
    local port=$(python3 -c "
import json
with open('$config_file') as f:
    config = json.load(f)
print(config['websocket']['port'])
")
    
    if [ "$port" -lt 1024 ] || [ "$port" -gt 65535 ]; then
        echo "WARNING: 端口 $port 不在推荐范围 [1024-65535]"
    fi
    
    echo "INFO: 配置文件健康检查通过"
}

# 定期检查配置
check_config_health "$REMOTE_CONTROLLER_CONFIG"
```

#### 配置变更通知

```bash
#!/bin/bash
# scripts/config_watcher.sh

# 监控配置文件变更
inotifywait -m -e modify "$REMOTE_CONTROLLER_CONFIG" |
while read path action file; do
    echo "$(date): 配置文件 $file 已修改"
    # 发送通知或记录日志
    logger "Remote Controller 配置文件已修改: $file"
done
```

### 6. 文档同步

确保配置文档与实际配置保持同步：

```bash
#!/bin/bash
# scripts/update_config_docs.sh

# 从配置文件生成文档
generate_config_docs() {
    local config_file="$1"
    local output_file="docs/config_$(basename $config_file .json).md"
    
    echo "# $(basename $config_file) 配置说明" > "$output_file"
    echo "" >> "$output_file"
    echo "配置文件: \`$config_file\`" >> "$output_file"
    echo "" >> "$output_file"
    echo '```json' >> "$output_file"
    cat "$config_file" >> "$output_file"
    echo '```' >> "$output_file"
}

# 为所有配置文件生成文档
for config in config/*.json; do
    generate_config_docs "$config"
done
```

---

通过遵循这些配置管理最佳实践，可以确保 Remote Controller 在不同环境中的稳定运行和易于维护。