# Adapter NodeBase 提取与 Go2 拆分 实施计划

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 从三个 adapter 中提取公共基类 `AdapterNodeBase`，拆分 `adapter_go2` 单文件为多文件架构，降低新 adapter 编写门槛。

**Architecture:** 在 `robot_adapter_interfaces` 库中新增 `AdapterNodeBase` 基类，处理 5 个标准服务注册和 YAML 配置加载。`adapter_go2` 拆分为 SDK 封装层 (`Go2SdkClient`) + 节点层 (`Go2AdapterNode`) + 入口 (`main.cpp`)。现有 `adapter_m20pro` 和 `adapter_fake` 迁移到基类以验证通用性。

**Tech Stack:** C++17, ROS2 Humble, ament_cmake, std_srvs::srv::Trigger, Unitree SDK2, nlohmann/json

**约束:** 不改变任何对外服务名称、topic 名称、YAML 配置格式。纯内部重构，行为等价。

---

## File Structure

### robot_adapter_interfaces (修改)

| 操作 | 文件路径 | 职责 |
|------|----------|------|
| Create | `include/robot_adapter_interfaces/adapter_node_base.hpp` | 基类声明：5 个标准服务虚函数、配置加载、扩展注册 |
| Create | `src/adapter_node_base.cpp` | 基类实现：服务注册、YAML 解析 |
| Modify | `CMakeLists.txt` | 新增 `adapter_node_base.cpp` 到库源文件，新增 `rcl_yaml_param_parser` 和 `ament_index_cpp` 依赖 |

### adapter_go2 (重构)

| 操作 | 文件路径 | 职责 |
|------|----------|------|
| Create | `include/adapter_go2/go2_sdk_client.hpp` | Unitree SDK2 封装声明 |
| Create | `src/go2_sdk_client.cpp` | SDK 初始化、运动指令、DDS 订阅 |
| Create | `include/adapter_go2/go2_adapter_node.hpp` | Go2AdapterNode 声明 |
| Create | `src/go2_adapter_node.cpp` | 标准服务回调 + 扩展服务 + cmd_vel + watchdog |
| Create | `src/main.cpp` | 入口 |
| Delete | `src/adapter_go2_node.cpp` | 被上述文件替代 |
| Modify | `CMakeLists.txt` | 更新源文件列表，新增 `robot_adapter_interfaces` 依赖 |

### adapter_m20pro (迁移)

| 操作 | 文件路径 | 职责 |
|------|----------|------|
| Rewrite | `src/adapter_m20pro_node.cpp` | 继承 `AdapterNodeBase`，仅实现 5 个虚函数 |
| Modify | `CMakeLists.txt` | 新增 `robot_adapter_interfaces` 依赖 |

### adapter_fake (迁移)

| 操作 | 文件路径 | 职责 |
|------|----------|------|
| Rewrite | `src/adapter_fake_node.cpp` | 继承 `AdapterNodeBase`，保留行为模式逻辑 |
| Modify | `CMakeLists.txt` | 新增 `robot_adapter_interfaces` 依赖 |

---

## Chunk 1: AdapterNodeBase 基类

### Task 1: 创建 AdapterNodeBase 头文件

**Files:**
- Create: `src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_node_base.hpp`

- [ ] **Step 1: 编写头文件**

```cpp
// adapter_node_base.hpp
#pragma once

#include <memory>
#include <rclcpp/rclcpp.hpp>
#include <rcl_yaml_param_parser/parser.h>
#include <std_srvs/srv/trigger.hpp>
#include <string>
#include <unordered_map>

namespace robot_adapter_interfaces {

class AdapterNodeBase : public rclcpp::Node {
public:
    using TriggerResponse = std::shared_ptr<std_srvs::srv::Trigger::Response>;

    /// @param adapter_type e.g. "go2" → node name "adapter_go2",
    ///   services at /adapter_go2/{connect,disconnect,safe_stop,health,system_info}
    /// @param package_name ament 包名，用于定位 config YAML。
    ///   默认为空，此时使用节点名 ("adapter_" + adapter_type) 作为包名。
    explicit AdapterNodeBase(const std::string& adapter_type,
                             const std::string& package_name = "");
    ~AdapterNodeBase() override = default;

    /// 两阶段初始化：在对象完全构造后、spin 前调用。
    /// 内部调用虚函数 RegisterExtensions()，此时派生类 vtable 已就绪。
    /// 所有 main.cpp 必须在 spin 前调用此方法。
    void Init();

protected:
    // --- 子类必须实现的 5 个标准服务 ---
    virtual void OnConnect(TriggerResponse response) = 0;
    virtual void OnDisconnect(TriggerResponse response) = 0;
    virtual void OnSafeStop(TriggerResponse response) = 0;
    virtual void OnHealth(TriggerResponse response) = 0;
    virtual void OnSystemInfo(TriggerResponse response) = 0;

    /// 子类可重写此方法注册额外的服务、订阅、定时器。
    /// 由 Init() 在对象完全构造后调用，虚函数分派正确。
    /// 无需在派生类构造函数中手动调用。
    virtual void RegisterExtensions() {}

    // --- 配置工具 ---
    /// 从 <package_share_dir>/config/<node_name>.yaml 加载参数
    void LoadConfigFromFile();

    template <typename T>
    T GetParamOrDefault(const std::string& name, const T& default_value) {
        auto it = config_overrides_.find(name);
        if (it != config_overrides_.end()) {
            try {
                return it->second.get_value<T>();
            } catch (const std::exception& e) {
                RCLCPP_WARN(get_logger(),
                            "Failed to convert param '%s': %s",
                            name.c_str(), e.what());
            }
        }
        return default_value;
    }

    template <typename T>
    T GetRequiredParam(const std::string& name) {
        auto it = config_overrides_.find(name);
        if (it != config_overrides_.end()) {
            try {
                return it->second.get_value<T>();
            } catch (const std::exception& e) {
                throw std::runtime_error(
                    "Failed to convert param '" + name + "': " + e.what());
            }
        }
        throw std::runtime_error(
            "Required parameter '" + name + "' not found in config");
    }

    const std::string& adapter_type() const { return adapter_type_; }

private:
    std::string adapter_type_;
    std::string package_name_;
    bool initialized_{false};
    std::unordered_map<std::string, rclcpp::Parameter> config_overrides_;

    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr connect_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr disconnect_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr safe_stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr health_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr system_info_srv_;
};

}  // namespace robot_adapter_interfaces
```

- [ ] **Step 2: Commit**

```bash
git add src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_node_base.hpp
git commit -m "feat(adapter_interfaces): add AdapterNodeBase header"
```

### Task 2: 实现 AdapterNodeBase

**Files:**
- Create: `src/robot_adapter_interfaces/src/adapter_node_base.cpp`
- Modify: `src/robot_adapter_interfaces/CMakeLists.txt`

- [ ] **Step 1: 编写实现文件**

```cpp
// adapter_node_base.cpp
#include "robot_adapter_interfaces/adapter_node_base.hpp"

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <filesystem>

namespace robot_adapter_interfaces {

AdapterNodeBase::AdapterNodeBase(const std::string& adapter_type,
                                 const std::string& package_name)
    : Node("adapter_" + adapter_type)
    , adapter_type_(adapter_type)
    , package_name_(package_name.empty()
                        ? "adapter_" + adapter_type
                        : package_name) {
    LoadConfigFromFile();

    const std::string prefix = "/" + std::string(get_name()) + "/";

    connect_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "connect",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse resp) { OnConnect(resp); });

    disconnect_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "disconnect",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse resp) { OnDisconnect(resp); });

    safe_stop_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "safe_stop",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse resp) { OnSafeStop(resp); });

    health_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "health",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse resp) { OnHealth(resp); });

    system_info_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "system_info",
        [this](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
               TriggerResponse resp) { OnSystemInfo(resp); });
}

void AdapterNodeBase::Init() {
    if (initialized_) return;
    initialized_ = true;
    RegisterExtensions();
    RCLCPP_INFO(get_logger(), "Init() complete, extensions registered");
}

void AdapterNodeBase::LoadConfigFromFile() {
    try {
        const std::string share_dir =
            ament_index_cpp::get_package_share_directory(package_name_);
        const std::string config_path =
            share_dir + "/config/" + std::string(get_name()) + ".yaml";

        if (!std::filesystem::exists(config_path)) {
            RCLCPP_INFO(get_logger(),
                        "Config not found at %s, using defaults",
                        config_path.c_str());
            return;
        }

        RCLCPP_INFO(get_logger(), "Loading config from %s",
                    config_path.c_str());

        rcl_params_t* params =
            rcl_yaml_node_struct_init(rcutils_get_default_allocator());
        if (!params) {
            RCLCPP_WARN(get_logger(), "Failed to init YAML parser");
            return;
        }

        if (!rcl_parse_yaml_file(config_path.c_str(), params)) {
            RCLCPP_WARN(get_logger(), "Failed to parse %s",
                        config_path.c_str());
            rcl_yaml_node_struct_fini(params);
            return;
        }

        for (size_t i = 0; i < params->num_nodes; ++i) {
            const char* node_name = params->node_names[i];
            if (!node_name) continue;

            std::string name(node_name);
            const size_t last_slash = name.find_last_of('/');
            if (last_slash != std::string::npos) {
                name = name.substr(last_slash + 1);
            }
            if (name != get_name()) continue;

            const rcl_node_params_t* np = &params->params[i];
            for (size_t j = 0; j < np->num_params; ++j) {
                const char* pname = np->parameter_names[j];
                const rcl_variant_t* pval = &np->parameter_values[j];

                rclcpp::Parameter param;
                if (pval->bool_value)
                    param = rclcpp::Parameter(pname, *pval->bool_value);
                else if (pval->integer_value)
                    param = rclcpp::Parameter(pname, *pval->integer_value);
                else if (pval->double_value)
                    param = rclcpp::Parameter(pname, *pval->double_value);
                else if (pval->string_value)
                    param = rclcpp::Parameter(pname,
                                              std::string(pval->string_value));
                else
                    continue;

                config_overrides_[pname] = param;
            }
        }

        rcl_yaml_node_struct_fini(params);
        RCLCPP_INFO(get_logger(), "Config loaded from %s",
                    config_path.c_str());
    } catch (const std::exception& e) {
        RCLCPP_WARN(get_logger(), "Config load failed: %s", e.what());
    }
}

}  // namespace robot_adapter_interfaces
```

- [ ] **Step 2: 更新 CMakeLists.txt**

修改 `src/robot_adapter_interfaces/CMakeLists.txt`：

```cmake
# 新增依赖
find_package(ament_index_cpp REQUIRED)
find_package(rcl_yaml_param_parser REQUIRED)

# 修改 add_library，新增源文件
add_library(${PROJECT_NAME}
  src/adapter_client.cpp
  src/adapter_node_base.cpp
  src/types.cpp
)

# 修改 ament_target_dependencies，新增依赖
ament_target_dependencies(${PROJECT_NAME}
  rclcpp
  std_srvs
  ament_index_cpp
  rcl_yaml_param_parser
)

# 修改 ament_export_dependencies，新增导出
ament_export_dependencies(rclcpp std_srvs ament_index_cpp rcl_yaml_param_parser)
```

- [ ] **Step 3: 更新 package.xml**

在 `src/robot_adapter_interfaces/package.xml` 的 `<depend>std_srvs</depend>` 之后新增：

```xml
  <depend>ament_index_cpp</depend>
  <depend>rcl_yaml_param_parser</depend>
```

- [ ] **Step 4: 构建验证**

```bash
cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
source /opt/ros/humble/setup.bash
colcon build --packages-select robot_adapter_interfaces
```

Expected: BUILD SUCCEEDED

- [ ] **Step 5: Commit**

```bash
git add src/robot_adapter_interfaces/src/adapter_node_base.cpp \
        src/robot_adapter_interfaces/CMakeLists.txt \
        src/robot_adapter_interfaces/package.xml
git commit -m "feat(adapter_interfaces): implement AdapterNodeBase with config loading"
```

### Task 3: 迁移 adapter_m20pro 到 AdapterNodeBase

**Files:**
- Rewrite: `src/adapter_m20pro/src/adapter_m20pro_node.cpp`
- Modify: `src/adapter_m20pro/CMakeLists.txt`

- [ ] **Step 1: 重写 adapter_m20pro_node.cpp**

```cpp
#include <memory>
#include <robot_adapter_interfaces/adapter_node_base.hpp>

namespace {

class M20ProAdapterNode
    : public robot_adapter_interfaces::AdapterNodeBase {
public:
    M20ProAdapterNode() : AdapterNodeBase("m20pro") {
        RCLCPP_INFO(get_logger(), "adapter_m20pro placeholder started");
    }

protected:
    void OnConnect(TriggerResponse response) override {
        response->success = false;
        response->message = "M20Pro adapter unavailable: not implemented yet";
    }

    void OnDisconnect(TriggerResponse response) override {
        response->success = true;
        response->message = "M20Pro placeholder disconnected";
    }

    void OnSafeStop(TriggerResponse response) override {
        response->success = true;
        response->message = "M20Pro placeholder safe_stop noop";
    }

    void OnHealth(TriggerResponse response) override {
        response->success = false;
        response->message = "M20Pro adapter unavailable: not implemented yet";
    }

    void OnSystemInfo(TriggerResponse response) override {
        response->success = true;
        response->message =
            R"({"adapter_type":"m20pro","status":"placeholder"})";
    }
};

}  // namespace

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<M20ProAdapterNode>();
    node->Init();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
```

- [ ] **Step 2: 更新 CMakeLists.txt**

修改 `src/adapter_m20pro/CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(adapter_m20pro)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)

add_executable(adapter_m20pro_node src/adapter_m20pro_node.cpp)
ament_target_dependencies(adapter_m20pro_node
  rclcpp std_srvs robot_adapter_interfaces)

install(TARGETS adapter_m20pro_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

ament_package()
```

- [ ] **Step 3: 更新 package.xml**

在 `src/adapter_m20pro/package.xml` 的 `<depend>std_srvs</depend>` 之后新增：

```xml
  <depend>robot_adapter_interfaces</depend>
```

- [ ] **Step 4: 构建验证**

```bash
colcon build --packages-select robot_adapter_interfaces adapter_m20pro
```

Expected: BUILD SUCCEEDED

- [ ] **Step 5: Commit**

```bash
git add src/adapter_m20pro/
git commit -m "refactor(adapter_m20pro): migrate to AdapterNodeBase"
```

### Task 4: 迁移 adapter_fake 到 AdapterNodeBase

**Files:**
- Rewrite: `src/adapter_fake/src/adapter_fake_node.cpp`
- Modify: `src/adapter_fake/CMakeLists.txt`

- [ ] **Step 1: 重写 adapter_fake_node.cpp**

保留所有行为模式逻辑，但继承 `AdapterNodeBase`，配置统一使用基类 `GetParamOrDefault`：

```cpp
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <robot_adapter_interfaces/adapter_node_base.hpp>
#include <thread>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_sigterm_received = 0;
int g_sigterm_delay_ms = 5000;

void DelayedSigtermHandler(int /*sig*/) {
    g_sigterm_received = 1;
}

class FakeAdapterNode
    : public robot_adapter_interfaces::AdapterNodeBase {
public:
    FakeAdapterNode() : AdapterNodeBase("fake") {
        behavior_mode_ =
            GetParamOrDefault<std::string>("behavior_mode", "normal");
        exit_delay_ms_ = GetParamOrDefault<int>("exit_delay_ms", 500);
        disconnect_hang_ms_ =
            GetParamOrDefault<int>("disconnect_hang_ms", 10000);
        sigterm_delay_ms_ =
            GetParamOrDefault<int>("sigterm_delay_ms", 5000);

        RCLCPP_INFO(get_logger(), "adapter_fake started, mode=%s",
                    behavior_mode_.c_str());
    }

    ~FakeAdapterNode() override {
        if (forked_child_pid_ > 0) {
            kill(forked_child_pid_, SIGKILL);
        }
    }

protected:
    void RegisterExtensions() override {
        // delayed_sigterm mode
        if (behavior_mode_ == "delayed_sigterm") {
            g_sigterm_delay_ms = sigterm_delay_ms_;
            struct sigaction sa{};
            sa.sa_handler = DelayedSigtermHandler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sigaction(SIGTERM, &sa, nullptr);

            sigterm_timer_ = create_wall_timer(
                std::chrono::milliseconds(50), [this]() {
                    if (g_sigterm_received) {
                        RCLCPP_INFO(get_logger(),
                                    "delayed_sigterm: sleeping %d ms",
                                    g_sigterm_delay_ms);
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(g_sigterm_delay_ms));
                        _exit(0);
                    }
                });
        }

        // exit_immediately mode
        if (behavior_mode_ == "exit_immediately") {
            exit_timer_ = create_wall_timer(
                std::chrono::milliseconds(exit_delay_ms_), [this]() {
                    RCLCPP_INFO(get_logger(),
                                "exit_immediately: exiting with code 42");
                    _exit(42);
                });
        }
    }

    void OnConnect(TriggerResponse response) override {
        if (behavior_mode_ == "connect_fail") {
            response->success = false;
            response->message =
                "fake connect failure (connect_fail mode)";
            return;
        }
        if (behavior_mode_ == "fork_child") {
            pid_t child = fork();
            if (child == 0) {
                while (true) sleep(3600);
            } else if (child > 0) {
                forked_child_pid_ = child;
                RCLCPP_INFO(get_logger(),
                            "fork_child: spawned child pid=%d", child);
            }
        }
        connected_ = true;
        response->success = true;
        response->message =
            "fake adapter connected (mode=" + behavior_mode_ + ")";
    }

    void OnDisconnect(TriggerResponse response) override {
        if (behavior_mode_ == "disconnect_hang") {
            RCLCPP_INFO(get_logger(), "disconnect_hang: blocking %d ms",
                        disconnect_hang_ms_);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(disconnect_hang_ms_));
        }
        connected_ = false;
        response->success = true;
        response->message = "fake adapter disconnected";
    }

    void OnSafeStop(TriggerResponse response) override {
        if (behavior_mode_ == "safe_stop_fail") {
            response->success = false;
            response->message =
                "fake safe_stop failure (safe_stop_fail mode)";
            return;
        }
        response->success = true;
        response->message = "fake safe_stop ok";
    }

    void OnHealth(TriggerResponse response) override {
        response->success = true;
        response->message =
            "{\"ready\":true,\"connected\":" +
            std::string(connected_ ? "true" : "false") +
            ",\"mode\":\"" + behavior_mode_ + "\"}";
    }

    void OnSystemInfo(TriggerResponse response) override {
        response->success = true;
        response->message =
            "{\"adapter\":\"fake\",\"mode\":\"" + behavior_mode_ +
            "\",\"connected\":" +
            std::string(connected_ ? "true" : "false") + "}";
    }

private:
    std::string behavior_mode_;
    int exit_delay_ms_{500};
    int disconnect_hang_ms_{10000};
    int sigterm_delay_ms_{5000};
    bool connected_{false};
    pid_t forked_child_pid_{-1};

    rclcpp::TimerBase::SharedPtr exit_timer_;
    rclcpp::TimerBase::SharedPtr sigterm_timer_;
};

}  // namespace

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<FakeAdapterNode>();
    node->Init();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
```

- [ ] **Step 2: 创建 config/adapter_fake.yaml**

创建 `src/adapter_fake/config/adapter_fake.yaml`，使基类 YAML 加载机制被 adapter_fake 验证到：

```yaml
adapter_fake:
    ros__parameters:
        behavior_mode: "normal"
        exit_delay_ms: 500
        disconnect_hang_ms: 10000
        sigterm_delay_ms: 5000
```

注意：运行时仍可通过 `--ros-args -p behavior_mode:=exit_immediately` 覆盖，因为 supervisor 可以在 execl 参数中追加 ROS args。但基类的 `LoadConfigFromFile()` 会先从此 YAML 加载默认值。

- [ ] **Step 3: 更新 CMakeLists.txt**

修改 `src/adapter_fake/CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(adapter_fake)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)

add_executable(adapter_fake_node src/adapter_fake_node.cpp)
ament_target_dependencies(adapter_fake_node
  rclcpp std_srvs robot_adapter_interfaces)

install(TARGETS adapter_fake_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

ament_package()
```

- [ ] **Step 4: 更新 package.xml**

在 `src/adapter_fake/package.xml` 的 `<depend>std_srvs</depend>` 之后新增：

```xml
  <depend>robot_adapter_interfaces</depend>
```

- [ ] **Step 5: 构建验证**

```bash
colcon build --packages-select robot_adapter_interfaces adapter_fake
```

Expected: BUILD SUCCEEDED

- [ ] **Step 6: Commit**

```bash
git add src/adapter_fake/
git commit -m "refactor(adapter_fake): migrate to AdapterNodeBase"
```

---

## Chunk 2: adapter_go2 拆分

### Task 5: 创建 Go2SdkClient

SDK 封装层，不依赖 ROS2，仅封装 Unitree SDK2 交互。

**Files:**
- Create: `src/adapter_go2/include/adapter_go2/go2_sdk_client.hpp`
- Create: `src/adapter_go2/src/go2_sdk_client.cpp`

- [ ] **Step 1: 编写 go2_sdk_client.hpp**

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <unitree/idl/go2/LowState_.hpp>
#include <unitree/idl/go2/SportModeState_.hpp>
#include <unitree/robot/go2/robot_state/robot_state_client.hpp>
#include <unitree/robot/go2/sport/sport_client.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

namespace adapter_go2 {

struct Go2SdkConfig {
    std::string network_interface;
    double sdk_timeout_sec = 10.0;
};

class Go2SdkClient {
public:
    using SportStateCallback =
        std::function<void(const unitree_go::msg::dds_::SportModeState_&)>;
    using LowStateCallback =
        std::function<void(const unitree_go::msg::dds_::LowState_&)>;

    Go2SdkClient() = default;
    ~Go2SdkClient() = default;

    // Non-copyable, non-movable (owns SDK singletons)
    Go2SdkClient(const Go2SdkClient&) = delete;
    Go2SdkClient& operator=(const Go2SdkClient&) = delete;

    /// Initialize SDK. Thread-safe, idempotent.
    /// @return true if initialized (or was already)
    bool Initialize(const Go2SdkConfig& config,
                    std::string* error = nullptr);
    bool IsInitialized() const;

    // --- Motion commands (all return SDK ret code, 0 = success) ---
    int32_t RecoveryStand();
    int32_t StopMove();
    int32_t StandDown();
    int32_t Damp();
    int32_t Move(float vx, float vy, float wz);

    // --- State queries ---
    int32_t GetServiceList(
        std::vector<unitree::robot::go2::ServiceState>& out);

    // --- DDS state callbacks ---
    void SetSportStateCallback(SportStateCallback cb);
    void SetLowStateCallback(LowStateCallback cb);

private:
    void OnSportStateRaw(const void* message);
    void OnLowStateRaw(const void* message);

    mutable std::mutex mutex_;
    bool initialized_ = false;

    std::unique_ptr<unitree::robot::go2::SportClient> sport_client_;
    std::unique_ptr<unitree::robot::go2::RobotStateClient>
        robot_state_client_;
    unitree::robot::ChannelSubscriberPtr<
        unitree_go::msg::dds_::SportModeState_>
        sport_state_sub_;
    unitree::robot::ChannelSubscriberPtr<
        unitree_go::msg::dds_::LowState_>
        low_state_sub_;

    SportStateCallback sport_state_cb_;
    LowStateCallback low_state_cb_;
};

}  // namespace adapter_go2
```

- [ ] **Step 2: 编写 go2_sdk_client.cpp**

```cpp
#include "adapter_go2/go2_sdk_client.hpp"

#include <unitree/robot/channel/channel_factory.hpp>

namespace adapter_go2 {

namespace {
constexpr const char* kSportStateTopic = "rt/sportmodestate";
constexpr const char* kLowStateTopic = "rt/lowstate";
}  // namespace

bool Go2SdkClient::Initialize(const Go2SdkConfig& config,
                               std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    try {
        unitree::robot::ChannelFactory::Instance()->Init(
            0, config.network_interface);

        sport_client_ =
            std::make_unique<unitree::robot::go2::SportClient>();
        sport_client_->SetTimeout(
            static_cast<float>(config.sdk_timeout_sec));
        sport_client_->Init();

        robot_state_client_ =
            std::make_unique<unitree::robot::go2::RobotStateClient>();
        robot_state_client_->SetTimeout(
            static_cast<float>(config.sdk_timeout_sec));
        robot_state_client_->Init();

        sport_state_sub_.reset(
            new unitree::robot::ChannelSubscriber<
                unitree_go::msg::dds_::SportModeState_>(
                kSportStateTopic));
        sport_state_sub_->InitChannel(
            std::bind(&Go2SdkClient::OnSportStateRaw, this,
                      std::placeholders::_1),
            1);

        low_state_sub_.reset(
            new unitree::robot::ChannelSubscriber<
                unitree_go::msg::dds_::LowState_>(kLowStateTopic));
        low_state_sub_->InitChannel(
            std::bind(&Go2SdkClient::OnLowStateRaw, this,
                      std::placeholders::_1),
            1);

        initialized_ = true;
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("SDK init failed: ") + e.what();
        return false;
    }
}

bool Go2SdkClient::IsInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

int32_t Go2SdkClient::RecoveryStand() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) return -1;
    return sport_client_->RecoveryStand();
}

int32_t Go2SdkClient::StopMove() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) return -1;
    return sport_client_->StopMove();
}

int32_t Go2SdkClient::StandDown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) return -1;
    return sport_client_->StandDown();
}

int32_t Go2SdkClient::Damp() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) return -1;
    return sport_client_->Damp();
}

int32_t Go2SdkClient::Move(float vx, float vy, float wz) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sport_client_) return -1;
    return sport_client_->Move(vx, vy, wz);
}

int32_t Go2SdkClient::GetServiceList(
    std::vector<unitree::robot::go2::ServiceState>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!robot_state_client_) return -1;
    return robot_state_client_->ServiceList(out);
}

void Go2SdkClient::SetSportStateCallback(SportStateCallback cb) {
    sport_state_cb_ = std::move(cb);
}

void Go2SdkClient::SetLowStateCallback(LowStateCallback cb) {
    low_state_cb_ = std::move(cb);
}

void Go2SdkClient::OnSportStateRaw(const void* message) {
    if (!message || !sport_state_cb_) return;
    sport_state_cb_(
        *static_cast<
            const unitree_go::msg::dds_::SportModeState_*>(message));
}

void Go2SdkClient::OnLowStateRaw(const void* message) {
    if (!message || !low_state_cb_) return;
    low_state_cb_(
        *static_cast<
            const unitree_go::msg::dds_::LowState_*>(message));
}

}  // namespace adapter_go2
```

- [ ] **Step 3: Commit**

```bash
git add src/adapter_go2/include/ src/adapter_go2/src/go2_sdk_client.cpp
git commit -m "feat(adapter_go2): extract Go2SdkClient from monolithic node"
```

### Task 6: 创建 Go2AdapterNode

继承 `AdapterNodeBase`，实现标准服务 + 扩展服务 + cmd_vel + watchdog。

**Files:**
- Create: `src/adapter_go2/include/adapter_go2/go2_adapter_node.hpp`
- Create: `src/adapter_go2/src/go2_adapter_node.cpp`

- [ ] **Step 1: 编写 go2_adapter_node.hpp**

```cpp
#pragma once

#include <chrono>
#include <geometry_msgs/msg/twist.hpp>
#include <mutex>
#include <robot_adapter_interfaces/adapter_node_base.hpp>
#include <string>

#include "adapter_go2/go2_sdk_client.hpp"

namespace adapter_go2 {

class Go2AdapterNode
    : public robot_adapter_interfaces::AdapterNodeBase {
public:
    Go2AdapterNode();

protected:
    void OnConnect(TriggerResponse response) override;
    void OnDisconnect(TriggerResponse response) override;
    void OnSafeStop(TriggerResponse response) override;
    void OnHealth(TriggerResponse response) override;
    void OnSystemInfo(TriggerResponse response) override;
    void RegisterExtensions() override;

private:
    enum class ControlState {
        kDisconnected,
        kConnectedIdle,
        kConnectedCommanding,
        kFault,
    };

    // Extension service callbacks
    void OnStand(TriggerResponse response);
    void OnStop(TriggerResponse response);
    void OnDamp(TriggerResponse response);
    void OnStopAndSit(TriggerResponse response);
    void OnEmergencyStop(TriggerResponse response);

    // cmd_vel
    void OnCmdVel(const geometry_msgs::msg::Twist::SharedPtr msg);
    void OnWatchdogTick();

    // Sport command helper
    template <typename Func>
    void ExecuteSportCommand(const std::string& name, Func&& call,
                             TriggerResponse response);

    // DDS state callbacks
    void OnSportState(
        const unitree_go::msg::dds_::SportModeState_& state);
    void OnLowState(const unitree_go::msg::dds_::LowState_& state);

    // Config
    double sdk_timeout_sec_{10.0};
    bool auto_stand_on_connect_{true};
    bool stand_down_on_disconnect_{false};
    double max_linear_x_{1.5};
    double max_linear_y_{1.0};
    double max_angular_z_{2.0};
    int cmd_vel_timeout_ms_{500};
    int watchdog_check_interval_ms_{100};
    std::string safe_stop_action_{"stop_move"};
    std::string network_interface_;

    // State — 锁模型说明：
    //   sdk_mutex_: 保护多步 SDK 操作序列的原子性（如 RecoveryStand + StopMove）
    //   node_state_mutex_: 保护节点级状态的读写一致性
    //   state_mutex_: 保护 DDS 缓存状态
    //   锁序：sdk_mutex_ → node_state_mutex_（禁止反向嵌套）
    std::mutex sdk_mutex_;
    std::mutex node_state_mutex_;
    std::mutex state_mutex_;
    Go2SdkClient sdk_;
    // 以下字段受 node_state_mutex_ 保护
    bool connected_{false};
    ControlState control_state_{ControlState::kDisconnected};
    std::string last_error_;
    std::chrono::steady_clock::time_point last_cmd_vel_time_{};

    // Cached DDS state
    unitree_go::msg::dds_::SportModeState_ latest_sport_state_;
    unitree_go::msg::dds_::LowState_ latest_low_state_;
    bool has_sport_state_{false};
    bool has_low_state_{false};

    // ROS2 handles
    rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr
        cmd_vel_sub_;
    rclcpp::TimerBase::SharedPtr watchdog_timer_;

    // Extension service handles
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stand_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr
        emergency_stop_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr
        stop_and_sit_srv_;
};

}  // namespace adapter_go2
```

- [ ] **Step 2: 编写 go2_adapter_node.cpp**

此文件包含所有回调实现。从原 `adapter_go2_node.cpp` 迁移逻辑，改为使用 `sdk_` 成员而非直接持有 SDK 对象。

关键变更点（相对于原文件）：
- 构造函数：通过 `AdapterNodeBase("go2")` 初始化，配置通过 `GetRequiredParam`/`GetParamOrDefault` 读取
- `EnsureSdkInitialized()` → `sdk_.Initialize(config, &error)`
- `sport_client_->XXX()` → `sdk_.XXX()`
- DDS 回调通过 `sdk_.SetSportStateCallback()` / `sdk_.SetLowStateCallback()` 注册
- 扩展服务在 `RegisterExtensions()` 中注册
- `OnHealth` / `OnSystemInfo` 中的 `GetServiceList` → `sdk_.GetServiceList()`

完整实现（从原文件逐函数迁移，此处列出关键结构）：

```cpp
#include "adapter_go2/go2_adapter_node.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace adapter_go2 {

namespace {
constexpr float kVelocityDeadband = 0.005f;
}

Go2AdapterNode::Go2AdapterNode() : AdapterNodeBase("go2") {
    network_interface_ =
        GetRequiredParam<std::string>("network_interface");
    sdk_timeout_sec_ =
        GetParamOrDefault<double>("sdk_timeout_sec", 10.0);
    auto_stand_on_connect_ =
        GetParamOrDefault<bool>("auto_stand_on_connect", true);
    stand_down_on_disconnect_ =
        GetParamOrDefault<bool>("stand_down_on_disconnect", false);
    max_linear_x_ =
        GetParamOrDefault<double>("max_linear_x", 1.5);
    max_linear_y_ =
        GetParamOrDefault<double>("max_linear_y", 1.0);
    max_angular_z_ =
        GetParamOrDefault<double>("max_angular_z", 2.0);
    cmd_vel_timeout_ms_ =
        GetParamOrDefault<int>("cmd_vel_timeout_ms", 500);
    watchdog_check_interval_ms_ =
        GetParamOrDefault<int>("watchdog_check_interval_ms", 100);
    safe_stop_action_ =
        GetParamOrDefault<std::string>("safe_stop_action", "stop_move");

    // Register DDS state callbacks
    sdk_.SetSportStateCallback(
        [this](const auto& s) { OnSportState(s); });
    sdk_.SetLowStateCallback(
        [this](const auto& s) { OnLowState(s); });

    RCLCPP_INFO(get_logger(),
        "adapter_go2 started. iface=%s auto_stand=%s "
        "cmd_vel_timeout=%dms safe_stop=%s",
        network_interface_.c_str(),
        auto_stand_on_connect_ ? "true" : "false",
        cmd_vel_timeout_ms_,
        safe_stop_action_.c_str());
}

void Go2AdapterNode::RegisterExtensions() {
    const std::string prefix =
        "/" + std::string(get_name()) + "/";

    stand_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "stand",
        [this](const auto&, TriggerResponse r) { OnStand(r); });

    stop_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "stop",
        [this](const auto&, TriggerResponse r) { OnStop(r); });

    emergency_stop_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "emergency_stop",
        [this](const auto&, TriggerResponse r) {
            OnEmergencyStop(r);
        });

    stop_and_sit_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "stop_and_sit",
        [this](const auto&, TriggerResponse r) {
            OnStopAndSit(r);
        });

    cmd_vel_sub_ = create_subscription<geometry_msgs::msg::Twist>(
        prefix + "cmd_vel", 10,
        [this](const auto& msg) { OnCmdVel(msg); });

    watchdog_timer_ = create_wall_timer(
        std::chrono::milliseconds(watchdog_check_interval_ms_),
        [this]() { OnWatchdogTick(); });
}

// --- 标准服务实现 ---

void Go2AdapterNode::OnConnect(TriggerResponse response) {
    Go2SdkConfig cfg{network_interface_, sdk_timeout_sec_};
    std::string init_error;
    if (!sdk_.Initialize(cfg, &init_error)) {
        response->success = false;
        response->message = init_error;
        return;
    }

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);

    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        if (connected_) {
            response->success = true;
            response->message = "GO2 already connected";
            return;
        }
    }

    try {
        if (auto_stand_on_connect_) {
            const int32_t stand_ret = sdk_.RecoveryStand();
            if (stand_ret != 0) {
                std::lock_guard<std::mutex> sl(node_state_mutex_);
                last_error_ = "stand command failed, ret=" +
                              std::to_string(stand_ret);
                response->success = false;
                response->message = "GO2 connect failed: " + last_error_;
                return;
            }
        }

        const int32_t stop_ret = sdk_.StopMove();
        if (stop_ret != 0) {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            last_error_ = "StopMove during connect failed, ret=" +
                          std::to_string(stop_ret);
            response->success = false;
            response->message = "GO2 connect failed: " + last_error_;
            return;
        }

        {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            connected_ = true;
            control_state_ = ControlState::kConnectedIdle;
            last_error_.clear();
        }
        response->success = true;
        response->message = "GO2 connected, iface=" + network_interface_;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        last_error_ = std::string("GO2 connect exception: ") + e.what();
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnDisconnect(TriggerResponse response) {
    Go2SdkConfig cfg{network_interface_, sdk_timeout_sec_};
    std::string init_error;
    if (!sdk_.Initialize(cfg, &init_error)) {
        response->success = false;
        response->message = init_error;
        return;
    }

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);

    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        if (!connected_) {
            response->success = true;
            response->message = "GO2 already disconnected";
            return;
        }
    }

    try {
        int32_t stop_ret = sdk_.StopMove();
        if (stop_ret != 0) {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            last_error_ = "disconnect: StopMove failed, ret=" +
                          std::to_string(stop_ret);
            response->success = false;
            response->message = last_error_;
            return;
        }

        if (stand_down_on_disconnect_) {
            int32_t sit_ret = sdk_.StandDown();
            if (sit_ret != 0) {
                std::lock_guard<std::mutex> sl(node_state_mutex_);
                last_error_ = "disconnect: StandDown failed, ret=" +
                              std::to_string(sit_ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
        }

        {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            connected_ = false;
            control_state_ = ControlState::kDisconnected;
        }
        response->success = true;
        response->message = "GO2 disconnected";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        connected_ = false;
        control_state_ = ControlState::kFault;
        last_error_ = std::string("GO2 disconnect exception: ") + e.what();
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnSafeStop(TriggerResponse response) {
    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        if (!connected_) {
            response->success = true;
            response->message = "not connected; safe_stop is a no-op";
            return;
        }
    }

    Go2SdkConfig cfg{network_interface_, sdk_timeout_sec_};
    std::string init_error;
    if (!sdk_.Initialize(cfg, &init_error)) {
        response->success = false;
        response->message = init_error;
        return;
    }

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);
    try {
        if (safe_stop_action_ == "damp") {
            const int32_t ret = sdk_.Damp();
            if (ret != 0) {
                std::lock_guard<std::mutex> sl(node_state_mutex_);
                last_error_ = "safe_stop damp failed, ret=" +
                              std::to_string(ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
        } else if (safe_stop_action_ == "stop_and_sit") {
            const int32_t stop_ret = sdk_.StopMove();
            if (stop_ret != 0) {
                std::lock_guard<std::mutex> sl(node_state_mutex_);
                last_error_ = "safe_stop StopMove failed, ret=" +
                              std::to_string(stop_ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
            const int32_t sit_ret = sdk_.StandDown();
            if (sit_ret != 0) {
                std::lock_guard<std::mutex> sl(node_state_mutex_);
                last_error_ = "safe_stop StandDown failed, ret=" +
                              std::to_string(sit_ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
        } else {
            const int32_t ret = sdk_.StopMove();
            if (ret != 0) {
                std::lock_guard<std::mutex> sl(node_state_mutex_);
                last_error_ = "safe_stop StopMove failed, ret=" +
                              std::to_string(ret);
                response->success = false;
                response->message = last_error_;
                return;
            }
        }

        {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            control_state_ = ControlState::kConnectedIdle;
        }
        response->success = true;
        response->message =
            "safe_stop success (action=" + safe_stop_action_ + ")";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        last_error_ = std::string("safe_stop exception: ") + e.what();
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnHealth(TriggerResponse response) {
    Go2SdkConfig cfg{network_interface_, sdk_timeout_sec_};
    std::string init_error;
    if (!sdk_.Initialize(cfg, &init_error)) {
        response->success = false;
        response->message = init_error;
        return;
    }

    std::vector<unitree::robot::go2::ServiceState> service_list;
    int32_t service_ret = sdk_.GetServiceList(service_list);

    // Snapshot DDS state
    bool has_sport = false;
    bool has_low = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        has_sport = has_sport_state_;
        has_low = has_low_state_;
    }

    // Snapshot node state — must hold node_state_mutex_ to avoid data race
    bool snap_connected;
    ControlState snap_control_state;
    std::string snap_last_error;
    std::chrono::steady_clock::time_point snap_last_cmd_vel;
    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        snap_connected = connected_;
        snap_control_state = control_state_;
        snap_last_error = last_error_;
        snap_last_cmd_vel = last_cmd_vel_time_;
    }

    nlohmann::json j;
    j["ready"] = true;
    j["connected"] = snap_connected;
    j["iface"] = network_interface_;
    j["service_list_ret"] = service_ret;
    j["service_count"] = service_list.size();
    j["has_sport_state"] = has_sport;
    j["has_low_state"] = has_low;
    j["cmd_vel_timeout_ms"] = cmd_vel_timeout_ms_;

    const char* cs_str = "unknown";
    switch (snap_control_state) {
    case ControlState::kDisconnected:        cs_str = "disconnected"; break;
    case ControlState::kConnectedIdle:       cs_str = "connected_idle"; break;
    case ControlState::kConnectedCommanding: cs_str = "connected_commanding"; break;
    case ControlState::kFault:               cs_str = "fault"; break;
    }
    j["control_state"] = cs_str;

    if (snap_connected &&
        snap_last_cmd_vel.time_since_epoch().count() > 0) {
        const auto ms_since =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - snap_last_cmd_vel)
                .count();
        j["ms_since_last_cmd_vel"] = ms_since;
    }

    if (!snap_last_error.empty()) {
        j["last_error"] = snap_last_error;
    }

    response->success = true;
    response->message = j.dump();
}

void Go2AdapterNode::OnSystemInfo(TriggerResponse response) {
    Go2SdkConfig cfg{network_interface_, sdk_timeout_sec_};
    std::string init_error;
    if (!sdk_.Initialize(cfg, &init_error)) {
        response->success = false;
        response->message = init_error;
        return;
    }

    unitree_go::msg::dds_::SportModeState_ sport;
    unitree_go::msg::dds_::LowState_ low;
    bool has_sport = false;
    bool has_low = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        sport = latest_sport_state_;
        low = latest_low_state_;
        has_sport = has_sport_state_;
        has_low = has_low_state_;
    }

    bool snap_connected;
    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        snap_connected = connected_;
    }

    std::vector<unitree::robot::go2::ServiceState> services;
    int32_t service_ret = sdk_.GetServiceList(services);

    nlohmann::json j;
    j["connected"] = snap_connected;
    j["network_interface"] = network_interface_;
    j["has_sport_state"] = has_sport;
    j["has_low_state"] = has_low;
    j["service_list_ret"] = service_ret;
    j["service_count"] = services.size();

    if (has_sport) {
        j["sport"] = {
            {"error_code", sport.error_code()},
            {"mode", static_cast<int>(sport.mode())},
            {"gait_type", static_cast<int>(sport.gait_type())},
            {"velocity", {sport.velocity()[0],
                          sport.velocity()[1],
                          sport.velocity()[2]}},
        };
    } else {
        j["sport"]["error"] = "no sport state yet";
    }

    if (has_low) {
        j["low"] = {
            {"battery_soc",
             static_cast<int>(low.bms_state().soc())},
            {"battery_current", low.bms_state().current()},
            {"battery_cycle", low.bms_state().cycle()},
            {"battery_status",
             static_cast<int>(low.bms_state().status())},
            {"battery_version",
             std::to_string(
                 static_cast<int>(low.bms_state().version_high())) +
                 "." +
                 std::to_string(
                     static_cast<int>(low.bms_state().version_low()))},
            {"battery_bq_ntc",
             {static_cast<int>(low.bms_state().bq_ntc()[0]),
              static_cast<int>(low.bms_state().bq_ntc()[1])}},
            {"battery_mcu_ntc",
             {static_cast<int>(low.bms_state().mcu_ntc()[0]),
              static_cast<int>(low.bms_state().mcu_ntc()[1])}},
            {"power_v", low.power_v()},
            {"power_a", low.power_a()},
        };
    } else {
        j["low"]["error"] = "no low state yet";
    }

    nlohmann::json services_json = nlohmann::json::array();
    for (const auto& svc : services) {
        services_json.push_back({
            {"name", svc.name},
            {"status", svc.status},
            {"protect", svc.protect},
        });
    }
    j["services"] = services_json;

    response->success = true;
    response->message = j.dump();
}

// --- 扩展服务实现 ---

template <typename Func>
void Go2AdapterNode::ExecuteSportCommand(
    const std::string& command_name, Func&& sdk_call,
    TriggerResponse response) {
    Go2SdkConfig cfg{network_interface_, sdk_timeout_sec_};
    std::string init_error;
    if (!sdk_.Initialize(cfg, &init_error)) {
        response->success = false;
        response->message = init_error;
        return;
    }

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);

    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        if (!connected_) {
            response->success = false;
            response->message =
                command_name + " rejected: GO2 not connected";
            return;
        }
    }

    try {
        const int32_t ret = sdk_call();
        if (ret != 0) {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            last_error_ = command_name + " failed, ret=" +
                          std::to_string(ret);
            response->success = false;
            response->message = last_error_;
            return;
        }
        response->success = true;
        response->message = command_name + " success";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        last_error_ =
            command_name + " exception: " + std::string(e.what());
        response->success = false;
        response->message = last_error_;
    }
}

void Go2AdapterNode::OnStand(TriggerResponse response) {
    ExecuteSportCommand(
        "stand", [this]() { return sdk_.RecoveryStand(); }, response);
}

void Go2AdapterNode::OnStop(TriggerResponse response) {
    ExecuteSportCommand(
        "stop", [this]() { return sdk_.StopMove(); }, response);
}

void Go2AdapterNode::OnDamp(TriggerResponse response) {
    ExecuteSportCommand(
        "damp", [this]() { return sdk_.Damp(); }, response);
}

// emergency_stop 映射到 Damp()，与原实现行为一致
void Go2AdapterNode::OnEmergencyStop(TriggerResponse response) {
    ExecuteSportCommand(
        "damp", [this]() { return sdk_.Damp(); }, response);
}

void Go2AdapterNode::OnStopAndSit(TriggerResponse response) {
    Go2SdkConfig cfg{network_interface_, sdk_timeout_sec_};
    std::string init_error;
    if (!sdk_.Initialize(cfg, &init_error)) {
        response->success = false;
        response->message = init_error;
        return;
    }

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);

    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        if (!connected_) {
            response->success = false;
            response->message = "stop_and_sit rejected: GO2 not connected";
            return;
        }
    }

    try {
        const int32_t stop_ret = sdk_.StopMove();
        if (stop_ret != 0) {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            last_error_ = "stop_and_sit: stop failed, ret=" +
                          std::to_string(stop_ret);
            response->success = false;
            response->message = last_error_;
            return;
        }

        const int32_t sit_ret = sdk_.StandDown();
        if (sit_ret != 0) {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            last_error_ = "stop_and_sit: sit failed, ret=" +
                          std::to_string(sit_ret);
            response->success = false;
            response->message = last_error_;
            return;
        }

        response->success = true;
        response->message = "stop_and_sit success";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        last_error_ =
            std::string("stop_and_sit exception: ") + e.what();
        response->success = false;
        response->message = last_error_;
    }
}

// --- cmd_vel + watchdog ---

void Go2AdapterNode::OnCmdVel(
    const geometry_msgs::msg::Twist::SharedPtr msg) {
    if (!msg) return;

    std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);

    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        if (!sdk_.IsInitialized() || !connected_) return;
        last_cmd_vel_time_ = std::chrono::steady_clock::now();
    }

    try {
        const float vx = std::clamp(
            static_cast<float>(msg->linear.x),
            -static_cast<float>(max_linear_x_),
            static_cast<float>(max_linear_x_));
        const float vy = std::clamp(
            static_cast<float>(msg->linear.y),
            -static_cast<float>(max_linear_y_),
            static_cast<float>(max_linear_y_));
        const float wz = std::clamp(
            static_cast<float>(msg->angular.z),
            -static_cast<float>(max_angular_z_),
            static_cast<float>(max_angular_z_));

        int32_t ret = 0;
        if (std::abs(vx) < kVelocityDeadband &&
            std::abs(vy) < kVelocityDeadband &&
            std::abs(wz) < kVelocityDeadband) {
            ret = sdk_.StopMove();
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            control_state_ = ControlState::kConnectedIdle;
        } else {
            ret = sdk_.Move(vx, vy, wz);
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            control_state_ = ControlState::kConnectedCommanding;
        }

        if (ret != 0) {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            last_error_ =
                "cmd_vel failed, ret=" + std::to_string(ret);
            RCLCPP_WARN(get_logger(),
                "cmd_vel failed ret=%d raw(vx=%.3f,vy=%.3f,wz=%.3f) "
                "clamped(vx=%.3f,vy=%.3f,wz=%.3f)",
                ret, msg->linear.x, msg->linear.y, msg->angular.z,
                vx, vy, wz);
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        last_error_ =
            std::string("cmd_vel exception: ") + e.what();
        RCLCPP_ERROR(get_logger(), "%s", last_error_.c_str());
    }
}

void Go2AdapterNode::OnWatchdogTick() {
    // Snapshot state without holding sdk_mutex_
    bool snap_connected;
    ControlState snap_control_state;
    std::chrono::steady_clock::time_point snap_last_cmd_vel;
    {
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        snap_connected = connected_;
        snap_control_state = control_state_;
        snap_last_cmd_vel = last_cmd_vel_time_;
    }

    if (!snap_connected || !sdk_.IsInitialized() ||
        snap_control_state != ControlState::kConnectedCommanding) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - snap_last_cmd_vel);

    if (elapsed.count() >= cmd_vel_timeout_ms_) {
        RCLCPP_WARN(get_logger(),
            "cmd_vel watchdog: no cmd_vel for %ldms, StopMove",
            static_cast<long>(elapsed.count()));
        std::lock_guard<std::mutex> sdk_lock(sdk_mutex_);
        try {
            const int32_t ret = sdk_.StopMove();
            if (ret != 0) {
                std::lock_guard<std::mutex> sl(node_state_mutex_);
                last_error_ = "watchdog StopMove failed, ret=" +
                              std::to_string(ret);
                RCLCPP_ERROR(get_logger(), "%s",
                             last_error_.c_str());
                return;
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> sl(node_state_mutex_);
            last_error_ = std::string("watchdog exception: ") +
                          e.what();
            RCLCPP_ERROR(get_logger(), "%s",
                         last_error_.c_str());
            return;
        }
        std::lock_guard<std::mutex> sl(node_state_mutex_);
        control_state_ = ControlState::kConnectedIdle;
    }
}

// --- DDS 回调 ---
void Go2AdapterNode::OnSportState(
    const unitree_go::msg::dds_::SportModeState_& state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_sport_state_ = state;
    has_sport_state_ = true;
}

void Go2AdapterNode::OnLowState(
    const unitree_go::msg::dds_::LowState_& state) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_low_state_ = state;
    has_low_state_ = true;
}

}  // namespace adapter_go2
```

锁模型说明：
- `sdk_mutex_`：保护多步 SDK 操作序列的原子性（如 `OnConnect` 中的 `RecoveryStand` + `StopMove`）
- `node_state_mutex_`：保护 `connected_`、`control_state_`、`last_error_`、`last_cmd_vel_time_` 的读写一致性
- `state_mutex_`：保护 DDS 缓存状态（`latest_sport_state_`、`latest_low_state_`）
- 锁序：`sdk_mutex_` → `node_state_mutex_`（禁止反向嵌套，避免死锁）
- `Go2SdkClient` 内部的 mutex 仅保护 SDK 对象的有效性，不保证跨调用的原子性

- [ ] **Step 3: Commit**

```bash
git add src/adapter_go2/include/adapter_go2/go2_adapter_node.hpp \
        src/adapter_go2/src/go2_adapter_node.cpp
git commit -m "feat(adapter_go2): create Go2AdapterNode inheriting AdapterNodeBase"
```

### Task 7: 创建 main.cpp 并更新 CMakeLists.txt

**Files:**
- Create: `src/adapter_go2/src/main.cpp`
- Delete: `src/adapter_go2/src/adapter_go2_node.cpp`
- Modify: `src/adapter_go2/CMakeLists.txt`

- [ ] **Step 1: 创建 main.cpp**

```cpp
#include <rclcpp/rclcpp.hpp>

#include "adapter_go2/go2_adapter_node.hpp"

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<adapter_go2::Go2AdapterNode>();
    node->Init();
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
```

- [ ] **Step 2: 更新 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)
project(adapter_go2)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_srvs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(robot_adapter_interfaces REQUIRED)

option(ADAPTER_GO2_USE_LOCAL_UNITREE_SDK2
  "Build adapter_go2 with bundled Unitree SDK2" ON)

if(ADAPTER_GO2_USE_LOCAL_UNITREE_SDK2)
  set(BUILD_EXAMPLES OFF CACHE BOOL "Build Unitree SDK examples" FORCE)
  add_subdirectory(unitree_sdk2)
endif()

add_executable(adapter_go2_node
  src/main.cpp
  src/go2_adapter_node.cpp
  src/go2_sdk_client.cpp
)

target_include_directories(adapter_go2_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

ament_target_dependencies(adapter_go2_node
  rclcpp std_srvs geometry_msgs robot_adapter_interfaces)

if(ADAPTER_GO2_USE_LOCAL_UNITREE_SDK2)
  target_link_libraries(adapter_go2_node unitree_sdk2)
  set_target_properties(adapter_go2_node PROPERTIES
    INSTALL_RPATH "$ORIGIN/..")
endif()
target_link_libraries(adapter_go2_node nlohmann_json::nlohmann_json)

install(TARGETS adapter_go2_node DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config DESTINATION share/${PROJECT_NAME})

ament_package()
```

注意：不再需要 `ament_index_cpp` 和 `rcl_yaml_param_parser` 直接依赖，因为这些已通过 `robot_adapter_interfaces` 传递导出。

- [ ] **Step 3: 删除旧文件**

```bash
git rm src/adapter_go2/src/adapter_go2_node.cpp
```

- [ ] **Step 4: 全量构建验证**

```bash
colcon build --packages-select robot_adapter_interfaces adapter_go2 adapter_m20pro adapter_fake
```

Expected: BUILD SUCCEEDED, 可执行文件路径不变 (`lib/adapter_go2/adapter_go2_node`)

- [ ] **Step 5: Commit**

```bash
git add src/adapter_go2/
git commit -m "refactor(adapter_go2): split monolithic node into sdk_client + adapter_node + main"
```

---

## Chunk 3: 验证与收尾

### Task 8: 全量构建 + 服务名验证

**Files:** 无新文件

- [ ] **Step 1: 全量构建**

```bash
cd /home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp
colcon build
```

Expected: 所有包 BUILD SUCCEEDED

- [ ] **Step 2: 验证服务名不变**

对每个 adapter 检查注册的服务名与重构前一致：

```bash
source install/setup.bash

# adapter_go2 应注册以下服务：
# /adapter_go2/connect
# /adapter_go2/disconnect
# /adapter_go2/safe_stop
# /adapter_go2/health
# /adapter_go2/system_info
# /adapter_go2/stand
# /adapter_go2/stop
# /adapter_go2/emergency_stop
# /adapter_go2/stop_and_sit
# 以及订阅 /adapter_go2/cmd_vel

# adapter_m20pro 应注册：
# /adapter_m20pro/connect
# /adapter_m20pro/disconnect
# /adapter_m20pro/safe_stop
# /adapter_m20pro/health
# /adapter_m20pro/system_info

# adapter_fake 应注册：
# /adapter_fake/connect
# /adapter_fake/disconnect
# /adapter_fake/safe_stop
# /adapter_fake/health
# /adapter_fake/system_info
```

可通过 `ros2 service list` 在节点运行时验证。

- [ ] **Step 3: 更新 CLAUDE.md**

在 `ros2_workspace_cpp/CLAUDE.md` 的 Architecture 部分追加：

```
**AdapterNodeBase** (in robot_adapter_interfaces) — base class for all adapter nodes.
Handles: 5 standard service registration, YAML config loading from `<package>/config/<node>.yaml`,
`GetParamOrDefault()`/`GetRequiredParam()` helpers, `RegisterExtensions()` hook for adapter-specific services.
Two-phase init: call `node->Init()` after construction in main.cpp to trigger `RegisterExtensions()`.
Package name defaults to node name; pass explicit `package_name` to constructor if they differ.
```

在 adapter_go2 部分更新文件结构说明。

- [ ] **Step 4: Final commit**

```bash
git add ros2_workspace_cpp/CLAUDE.md
git commit -m "docs: update CLAUDE.md with AdapterNodeBase architecture"
```
