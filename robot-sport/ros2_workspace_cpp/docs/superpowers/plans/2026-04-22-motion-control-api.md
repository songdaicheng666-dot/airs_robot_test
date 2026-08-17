# Motion Control API Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `POST /motion?motion_id=<id>` to `robot_switch_server` that dispatches caller-named discrete actions to the currently running adapter, using a motion set each adapter declares in `/system_info`.

**Architecture:** `SystemInfoBuilder` gains a typed `SetMotions(vector<MotionDescriptor>)` API. The switch server fetches `/system_info` at the end of `Start()`, parses the `motions` array, caches it in `RunningAdapter::motions` (id → descriptor), and dispatches via a new generic `AdapterClient::CallTriggerByName(suffix)` that reuses the same dedicated RPC node/executor as the five fixed methods. Motion set is cleared on `Stop()` or crash — no runtime mutation.

**Tech Stack:** C++17, ROS2 Humble (rclcpp, `std_srvs/srv/Trigger`), nlohmann_json, cpp-httplib, colcon build.

**Reference spec:** `docs/superpowers/specs/2026-04-22-motion-control-api-design.md` (commit `1b47361`).

**Workspace convention:** No unit-test framework at workspace level, no linter. Tasks verify via `colcon build` at each step and a final live integration walk against `adapter_fake`. No new test framework introduced.

---

## File Structure

**Create:** none.

**Modify:**

| File | Change |
|---|---|
| `src/robot_adapter_interfaces/include/robot_adapter_interfaces/system_info.hpp` | Add `MotionDescriptor` struct; add `SetMotions`/`motions_` to builder |
| `src/robot_adapter_interfaces/src/system_info.cpp` | Emit `"motions"` key in `Build()`; validate entries |
| `src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_client.hpp` | Add `CallTriggerByName` + dynamic client cache members |
| `src/robot_adapter_interfaces/src/adapter_client.cpp` | Implement `CallTriggerByName` |
| `src/robot_switch_server/include/robot_switch_server/core/adapter_runtime_manager.hpp` | Add `InvokeMotionResult`, `InvokeMotion`, `motions` field on `RunningAdapter` |
| `src/robot_switch_server/src/core/adapter_runtime_manager.cpp` | Populate motion cache after Connect; implement `InvokeMotion`; add `ParseMotionsFromSystemInfo` and `IsValidMotionId` helpers |
| `src/robot_switch_server/include/robot_switch_server/http/json_response_builder.hpp` | Add `BuildMotionResponse` declaration |
| `src/robot_switch_server/src/http/json_response_builder.cpp` | Implement `BuildMotionResponse` |
| `src/robot_switch_server/src/infra/http_server_runner_httplib.cpp` | Register `POST /motion` route |
| `src/adapter_fake/src/adapter_fake_node.cpp` | Register `echo`/`fail_motion` services; declare motion set in `OnSystemInfo` |
| `src/adapter_go2/src/go2_adapter_node.cpp` | Declare motion set in `OnSystemInfo` |
| `src/adapter_lynx/src/lynx_adapter_node.cpp` | Declare motion set in `OnSystemInfo` |

**No CMake changes** — all modifications are to files already listed in their package's `CMakeLists.txt`.

---

## Conventions

- **Build command used in every task:**
  ```bash
  source /opt/ros/humble/setup.bash && \
    colcon build --packages-select <packages listed in the task>
  ```
- **Commit message style:** match existing `feat(pkg): ...`, `fix(pkg): ...`. Commits are one per task.
- **Style:** match surrounding code — 4-space indent, `namespace` blocks, `rclcpp::Logger` via `node_->get_logger()`, error paths use `SetLastResult` / `MakeError` / `MakeSuccess` helpers already in `adapter_runtime_manager.cpp`.

---

### Task 1: Extend `SystemInfoBuilder` with typed motion set

**Files:**
- Modify: `src/robot_adapter_interfaces/include/robot_adapter_interfaces/system_info.hpp`
- Modify: `src/robot_adapter_interfaces/src/system_info.cpp`

- [ ] **Step 1: Add `MotionDescriptor` struct and `SetMotions` to the header**

In `system_info.hpp`, after the existing `MotionState` struct (around line 13) add the new struct, and add the `SetMotions` method plus `motions_` member to the class. Final file:

```cpp
#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace robot_adapter_interfaces {

struct MotionState {
    double x{0.0};
    double y{0.0};
    double yaw{0.0};
};

struct MotionDescriptor {
    std::string id;              // [A-Za-z0-9_]+, non-empty
    std::string service_suffix;  // appended to adapter service_prefix (e.g. "stop_and_sit")
    std::string description;     // optional UX label; "" if none
};

class SystemInfoBuilder {
public:
    using BatteryValue = std::variant<int, std::vector<int>>;

    SystemInfoBuilder& SetBattery(int battery_percentage);
    SystemInfoBuilder& SetBattery(std::vector<int> battery_percentages);
    SystemInfoBuilder& SetMotion(double x, double y, double yaw);
    SystemInfoBuilder& SetMotion(const MotionState& motion);
    SystemInfoBuilder& SetDetailsJson(std::string details_json);
    SystemInfoBuilder& SetMotions(std::vector<MotionDescriptor> motions);

    [[nodiscard]] std::string Build() const;

private:
    std::optional<BatteryValue> battery_;
    std::optional<MotionState> motion_;
    std::string details_json_;
    std::optional<std::vector<MotionDescriptor>> motions_;
};

}  // namespace robot_adapter_interfaces
```

- [ ] **Step 2: Implement `SetMotions` and `Build` motion output**

Replace the full contents of `system_info.cpp` with:

```cpp
#include "robot_adapter_interfaces/system_info.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <unordered_set>
#include <utility>

namespace robot_adapter_interfaces {

namespace {

nlohmann::json ParseDetailsObject(const std::string& details_json) {
    if (details_json.empty()) {
        return nlohmann::json::object();
    }

    const auto parsed = nlohmann::json::parse(details_json, nullptr, false);
    if (parsed.is_discarded()) {
        return nlohmann::json{{"_raw", details_json}};
    }

    if (parsed.is_object()) {
        return parsed;
    }

    return nlohmann::json{{"_raw", parsed}};
}

nlohmann::json ToBatteryJson(const SystemInfoBuilder::BatteryValue& battery) {
    if (std::holds_alternative<int>(battery)) {
        return nlohmann::json(std::get<int>(battery));
    }
    return nlohmann::json(std::get<std::vector<int>>(battery));
}

nlohmann::json ToMotionJson(const MotionState& motion) {
    return nlohmann::json{
        {"x", motion.x},
        {"y", motion.y},
        {"yaw", motion.yaw},
    };
}

bool IsValidMotionIdCharset(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

nlohmann::json ToMotionsJson(const std::vector<MotionDescriptor>& motions) {
    nlohmann::json arr = nlohmann::json::array();
    std::unordered_set<std::string> seen;
    for (const auto& m : motions) {
        if (!IsValidMotionIdCharset(m.id)) {
            std::cerr << "[SystemInfoBuilder] dropping motion: invalid id '"
                      << m.id << "'" << std::endl;
            continue;
        }
        if (m.service_suffix.empty()) {
            std::cerr << "[SystemInfoBuilder] dropping motion '" << m.id
                      << "': empty service_suffix" << std::endl;
            continue;
        }
        if (!seen.insert(m.id).second) {
            std::cerr << "[SystemInfoBuilder] dropping motion '" << m.id
                      << "': duplicate id" << std::endl;
            continue;
        }
        arr.push_back({
            {"id", m.id},
            {"service_suffix", m.service_suffix},
            {"description", m.description},
        });
    }
    return arr;
}

}  // namespace

SystemInfoBuilder& SystemInfoBuilder::SetBattery(int battery_percentage) {
    battery_ = battery_percentage;
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetBattery(
    std::vector<int> battery_percentages) {
    battery_ = std::move(battery_percentages);
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetMotion(double x, double y, double yaw) {
    motion_ = MotionState{x, y, yaw};
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetMotion(const MotionState& motion) {
    motion_ = motion;
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetDetailsJson(std::string details_json) {
    details_json_ = std::move(details_json);
    return *this;
}

SystemInfoBuilder& SystemInfoBuilder::SetMotions(
    std::vector<MotionDescriptor> motions) {
    motions_ = std::move(motions);
    return *this;
}

std::string SystemInfoBuilder::Build() const {
    nlohmann::json result = nlohmann::json::object();

    if (battery_.has_value()) {
        result["battery"] = ToBatteryJson(*battery_);
    } else {
        result["battery"] = nullptr;
    }

    if (motion_.has_value()) {
        result["motion"] = ToMotionJson(*motion_);
    } else {
        result["motion"] = nullptr;
    }

    if (motions_.has_value()) {
        result["motions"] = ToMotionsJson(*motions_);
    }

    result["details"] = ParseDetailsObject(details_json_);
    return result.dump();
}

}  // namespace robot_adapter_interfaces
```

Key design points in this file:
- `"motions"` key is **absent** when `SetMotions` was never called — adapters that don't opt in are byte-identical to today.
- Validation is best-effort with `std::cerr` warnings (no logger dep in this package). Silent-drop for invalid entries matches the style of `ParseDetailsObject`.
- First-occurrence-wins deduplication.

- [ ] **Step 3: Build the interfaces package**

Run:

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select robot_adapter_interfaces
```

Expected: success, no warnings from this file. If the build complains about an unused variable, re-check the ToMotionsJson helper.

- [ ] **Step 4: Commit**

```bash
git add src/robot_adapter_interfaces/include/robot_adapter_interfaces/system_info.hpp \
        src/robot_adapter_interfaces/src/system_info.cpp
git commit -m "feat(robot_adapter_interfaces): add MotionDescriptor and SetMotions to SystemInfoBuilder"
```

---

### Task 2: Add generic `CallTriggerByName` to `AdapterClient`

**Files:**
- Modify: `src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_client.hpp`
- Modify: `src/robot_adapter_interfaces/src/adapter_client.cpp`

**Design note:** the spec mentioned a `timeout` parameter, but the existing `CallTrigger` helper uses the class-level `call_timeout_` member. Passing a per-call timeout would require duplicating the wait logic. Since the manager only ever passes its own `call_timeout_` (the same value used to construct the client), we drop the parameter and reuse the class member. This matches the shape of the five existing methods (`Connect()`, `SystemInfo()`, etc.).

- [ ] **Step 1: Add declarations to the header**

In `adapter_client.hpp`, add an `<unordered_map>` include, the public `CallTriggerByName` method, and two private members for the dynamic client cache. Full file:

```cpp
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <string>
#include <thread>
#include <unordered_map>

namespace robot_adapter_interfaces {

struct AdapterCallResult {
    bool ok{false};
    bool reachable{false};
    std::string message;
};

class AdapterClient {
public:
    AdapterClient(
        rclcpp::Node::SharedPtr parent_node, std::string adapter_name,
        std::string service_prefix,
        std::chrono::milliseconds service_wait = std::chrono::milliseconds(500),
        std::chrono::milliseconds call_timeout =
            std::chrono::milliseconds(1200));

    ~AdapterClient();

    AdapterCallResult Connect();
    AdapterCallResult Disconnect();
    AdapterCallResult SafeStop();
    AdapterCallResult Health();
    AdapterCallResult SystemInfo();

    // Generic Trigger call by service suffix. The full service path is
    // `service_prefix + "/" + service_suffix`. The underlying client is created
    // on first use and cached for subsequent calls with the same suffix.
    // Timeout matches `call_timeout` from construction.
    AdapterCallResult CallTriggerByName(const std::string& service_suffix);

    const std::string& name() const { return adapter_name_; }

private:
    AdapterCallResult CallTrigger(
        const std::string& action,
        const rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr& client);

    void SpinThread();

    std::string adapter_name_;
    std::string service_prefix_;
    std::chrono::milliseconds service_wait_;
    std::chrono::milliseconds call_timeout_;
    std::mutex call_mutex_;

    // Independent node + executor (avoids executor conflict with main node)
    rclcpp::Node::SharedPtr rpc_node_;
    rclcpp::executors::SingleThreadedExecutor executor_;
    std::thread spin_thread_;
    std::atomic<bool> shutdown_{false};

    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr connect_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr disconnect_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr safe_stop_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr health_client_;
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr system_info_client_;

    // Cache of clients created on demand by CallTriggerByName.
    std::mutex dynamic_clients_mutex_;
    std::unordered_map<std::string,
                       rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr>
        dynamic_clients_;
};

}  // namespace robot_adapter_interfaces
```

- [ ] **Step 2: Implement `CallTriggerByName`**

Append to `adapter_client.cpp`, after the `SystemInfo()` method (around the end, before the existing `CallTrigger` helper):

```cpp
AdapterCallResult AdapterClient::CallTriggerByName(
    const std::string& service_suffix) {
    rclcpp::Client<std_srvs::srv::Trigger>::SharedPtr client;
    {
        std::lock_guard<std::mutex> lock(dynamic_clients_mutex_);
        auto it = dynamic_clients_.find(service_suffix);
        if (it == dynamic_clients_.end()) {
            client = rpc_node_->create_client<std_srvs::srv::Trigger>(
                service_prefix_ + "/" + service_suffix);
            dynamic_clients_.emplace(service_suffix, client);
        } else {
            client = it->second;
        }
    }
    return CallTrigger(service_suffix, client);
}
```

Lock ordering note: `dynamic_clients_mutex_` is released before `CallTrigger` acquires `call_mutex_`. Both locks are leaf-level — no nested acquisition.

- [ ] **Step 3: Build**

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select robot_adapter_interfaces
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
git add src/robot_adapter_interfaces/include/robot_adapter_interfaces/adapter_client.hpp \
        src/robot_adapter_interfaces/src/adapter_client.cpp
git commit -m "feat(robot_adapter_interfaces): add AdapterClient::CallTriggerByName for generic Trigger dispatch"
```

---

### Task 3: Motion cache and `InvokeMotion` in `AdapterRuntimeManager`

**Files:**
- Modify: `src/robot_switch_server/include/robot_switch_server/core/adapter_runtime_manager.hpp`
- Modify: `src/robot_switch_server/src/core/adapter_runtime_manager.cpp`

**Design note on return shape:** `OperationResult.code` is an `ErrorCode` enum with values 0–6. The motion response envelope uses 0/400/502 — HTTP-status-flavored codes. Rather than polluting `ErrorCode` with HTTP semantics, `InvokeMotionResult` is a flat struct carrying a plain `int code` directly; the HTTP handler and `BuildMotionResponse` consume it without touching `OperationResult`/`ErrorCode`. This deviates from the spec's "reuses existing `{code, msg, detail}`" phrasing but preserves the **shape** (`code`, `msg`, `detail`) and keeps the interfaces package unchanged.

- [ ] **Step 1: Extend the header — new result struct, method, and `motions` field**

In `adapter_runtime_manager.hpp`:

1. Add `#include <unordered_map>` to the includes (near the existing `<set>` include).
2. Inside the public `AdapterRuntimeManager` class, after the existing `OperationWithSnapshot` struct (around line 32), add:

   ```cpp
   struct InvokeMotionResult {
       int code{0};                // 0 success, 400 bad request, 502 adapter failure
       std::string message;
       std::string detail;
       std::string motion_id;
   };
   ```

3. In the public methods block, after `GetEnabledAdapterTypes()` (around line 45), add:

   ```cpp
   [[nodiscard]] InvokeMotionResult InvokeMotion(const std::string& motion_id);
   ```

4. In the private `RunningAdapter` struct (around line 57), add the motions field:

   ```cpp
   struct RunningAdapter {
       AdapterSpec spec;
       std::string executable_path;
       pid_t pid{-1};
       std::shared_ptr<robot_adapter_interfaces::AdapterClient> client;
       std::unordered_map<std::string,
                          robot_adapter_interfaces::MotionDescriptor>
           motions;
   };
   ```

5. Make sure the `MotionDescriptor` type is visible — `adapter_client.hpp` is already included, but `MotionDescriptor` lives in `system_info.hpp`. Add `#include "robot_adapter_interfaces/system_info.hpp"` to the top of the header next to the other `robot_adapter_interfaces/...` includes.

- [ ] **Step 2: Add parse and validate helpers to the cpp anonymous namespace**

Open `adapter_runtime_manager.cpp`. Two edits:

**2a. Add the include** at the top of the file, alongside the other `#include` lines:

```cpp
#include <nlohmann/json.hpp>
```

(`nlohmann_json` is already linked transitively via `robot_adapter_interfaces`'s `ament_export_dependencies`, so no CMake change is required.)

**2b. Add the helpers** inside the file-scope anonymous namespace (the `namespace { ... }` block near the top of the file, just inside `namespace robot_switch_server { ... }`). If no anonymous namespace exists yet, create one right after the opening `namespace robot_switch_server {`:

```cpp
bool IsValidMotionIdCharset(const std::string& id) {
    if (id.empty()) {
        return false;
    }
    for (const char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::unordered_map<std::string, robot_adapter_interfaces::MotionDescriptor>
ParseMotionsFromSystemInfo(const std::string& payload_json,
                           const rclcpp::Logger& logger) {
    std::unordered_map<std::string, robot_adapter_interfaces::MotionDescriptor>
        map;
    if (payload_json.empty()) {
        return map;
    }

    const auto parsed = nlohmann::json::parse(payload_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        RCLCPP_WARN(logger,
                    "motion cache: system_info payload not a JSON object; "
                    "motion set empty");
        return map;
    }

    const auto it = parsed.find("motions");
    if (it == parsed.end() || !it->is_array()) {
        return map;  // Adapter simply declared no motions; normal.
    }

    for (const auto& entry : *it) {
        if (!entry.is_object()) {
            RCLCPP_WARN(logger, "motion cache: entry not an object; dropped");
            continue;
        }
        robot_adapter_interfaces::MotionDescriptor desc;
        if (entry.contains("id") && entry.at("id").is_string()) {
            desc.id = entry.at("id").get<std::string>();
        }
        if (entry.contains("service_suffix") &&
            entry.at("service_suffix").is_string()) {
            desc.service_suffix =
                entry.at("service_suffix").get<std::string>();
        }
        if (entry.contains("description") &&
            entry.at("description").is_string()) {
            desc.description = entry.at("description").get<std::string>();
        }

        if (!IsValidMotionIdCharset(desc.id)) {
            RCLCPP_WARN(logger,
                        "motion cache: invalid id '%s' dropped",
                        desc.id.c_str());
            continue;
        }
        if (desc.service_suffix.empty()) {
            RCLCPP_WARN(logger,
                        "motion cache: id '%s' has empty service_suffix; dropped",
                        desc.id.c_str());
            continue;
        }
        if (map.count(desc.id) != 0) {
            RCLCPP_WARN(logger,
                        "motion cache: duplicate id '%s' dropped",
                        desc.id.c_str());
            continue;
        }
        map.emplace(desc.id, std::move(desc));
    }
    return map;
}
```

Also ensure `<unordered_map>` is included at the top of the file (it may already be transitively, but add an explicit `#include <unordered_map>` if not).

- [ ] **Step 3: Populate the cache after Connect succeeds in `Start()`**

In `adapter_runtime_manager.cpp`, locate the `Start()` method. Just after the connect-result-success check and before the `// Success: commit state under lock` block, insert the system-info fetch. The edited section (full `Start()` with the new block visible):

```cpp
AdapterRuntimeManager::OperationWithSnapshot AdapterRuntimeManager::Start(
    const std::string& adapter_type) {
    const std::string normalized_type = NormalizeAdapterType(adapter_type);

    if (const auto error = ValidateStartConditions(normalized_type)) {
        SetLastResult(*error);
        return {*error, GetStatusWithHealth()};
    }

    auto [running_opt, launch_result] = TryLaunchAdapter(normalized_type);
    if (!running_opt.has_value()) {
        SetLastResult(launch_result);
        return {launch_result, GetStatusWithHealth()};
    }

    const auto connect_result = WaitForAdapterReadyAndConnect(*running_opt);
    if (!connect_result.success) {
        auto result = HandleConnectFailure(*running_opt, connect_result);
        SetLastResult(result);
        return {result, GetStatusWithHealth()};
    }

    // Populate motion cache before committing state. Warn-and-continue on
    // failure — motion declaration is opt-in and non-critical to adapter health.
    {
        const auto si = running_opt->client->SystemInfo();
        if (si.ok) {
            running_opt->motions =
                ParseMotionsFromSystemInfo(si.message, node_->get_logger());
        } else {
            RCLCPP_WARN(
                node_->get_logger(),
                "motion cache: SystemInfo call failed at startup; "
                "motion set empty (%s)",
                si.message.c_str());
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = std::move(*running_opt);
        state_machine_.ProcessEvent(AdapterEvent::kStartSucceeded);
    }

    auto result = MakeSuccess("adapter started and connected",
                             BuildErrorDetail("adapter_type=", normalized_type,
                                             "; pid=", std::to_string(running_->pid)));
    SetLastResult(result);
    return {result, GetStatusWithHealth()};
}
```

Note: `Stop()` requires **no changes** — `running_.reset()` already destroys the map along with the rest of `RunningAdapter`.

- [ ] **Step 4: Implement `InvokeMotion`**

Append to `adapter_runtime_manager.cpp`, after the existing `GetEnabledAdapterTypes()` implementation:

```cpp
AdapterRuntimeManager::InvokeMotionResult AdapterRuntimeManager::InvokeMotion(
    const std::string& motion_id) {
    std::shared_ptr<robot_adapter_interfaces::AdapterClient> client;
    robot_adapter_interfaces::MotionDescriptor desc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RefreshCrashedProcessState();

        if (!running_.has_value()) {
            return {400, "no adapter running", "", motion_id};
        }
        if (state_machine_.state() != AdapterState::kRunning) {
            return {400,
                    "adapter not ready (state=" +
                        AdapterStateMachine::ToString(state_machine_.state()) +
                        ")",
                    "", motion_id};
        }
        if (!IsValidMotionIdCharset(motion_id)) {
            return {400, "invalid motion_id", "", motion_id};
        }
        const auto it = running_->motions.find(motion_id);
        if (it == running_->motions.end()) {
            return {400, "unknown motion_id '" + motion_id + "'", "",
                    motion_id};
        }
        client = running_->client;
        desc = it->second;
    }

    // RPC outside the lock.
    const auto rpc = client->CallTriggerByName(desc.service_suffix);
    if (rpc.ok) {
        return {0, "success", rpc.message, motion_id};
    }
    if (rpc.reachable) {
        return {502, "adapter rejected motion", rpc.message, motion_id};
    }
    return {502, "adapter call failed", rpc.message, motion_id};
}
```

- [ ] **Step 5: Build**

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select robot_adapter_interfaces robot_switch_server
```

Expected: success. If the compiler complains about an incomplete `MotionDescriptor` type, confirm Step 1's include of `system_info.hpp`. If it complains about `AdapterStateMachine::ToString`, confirm the symbol is already exported from `adapter_state_machine.hpp` (it is, per the survey).

- [ ] **Step 6: Commit**

```bash
git add src/robot_switch_server/include/robot_switch_server/core/adapter_runtime_manager.hpp \
        src/robot_switch_server/src/core/adapter_runtime_manager.cpp
git commit -m "feat(robot_switch_server): add motion cache and InvokeMotion dispatch"
```

---

### Task 4: `BuildMotionResponse` in `JsonResponseBuilder`

**Files:**
- Modify: `src/robot_switch_server/include/robot_switch_server/http/json_response_builder.hpp`
- Modify: `src/robot_switch_server/src/http/json_response_builder.cpp`

- [ ] **Step 1: Declare `BuildMotionResponse`**

In `json_response_builder.hpp`, after `BuildAdaptersResponse` (around line 35), add:

```cpp
    // Build response for /motion endpoint
    static std::string BuildMotionResponse(
        const AdapterRuntimeManager::InvokeMotionResult& result);
```

The `AdapterRuntimeManager` header is already included at the top of the file, so no include change is needed.

- [ ] **Step 2: Implement `BuildMotionResponse`**

In `json_response_builder.cpp`, append before the closing `}  // namespace robot_switch_server`:

```cpp
std::string JsonResponseBuilder::BuildMotionResponse(
    const AdapterRuntimeManager::InvokeMotionResult& result) {
    if (result.code == 0 || result.code == 502) {
        const std::string data = JsonBuilder{256}
            .Add("motion_id", result.motion_id)
            .Add("detail", result.detail)
            .Build();
        return Envelope(static_cast<int64_t>(result.code), result.message,
                        data);
    }
    // 400 and anything else with no useful data payload.
    return EnvelopeNull(static_cast<int64_t>(result.code), result.message);
}
```

The `Envelope` and `EnvelopeNull` helpers already exist in the anonymous namespace at the top of this file.

- [ ] **Step 3: Build**

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select robot_switch_server
```

Expected: success.

- [ ] **Step 4: Commit**

```bash
git add src/robot_switch_server/include/robot_switch_server/http/json_response_builder.hpp \
        src/robot_switch_server/src/http/json_response_builder.cpp
git commit -m "feat(robot_switch_server): add BuildMotionResponse"
```

---

### Task 5: Register `POST /motion` route

**Files:**
- Modify: `src/robot_switch_server/src/infra/http_server_runner_httplib.cpp`

- [ ] **Step 1: Add the `/motion` route inside `RegisterRoutes`**

In `http_server_runner_httplib.cpp`, inside the `RegisterRoutes` function, immediately after the existing `/stop` route (around line 107) and before `/system_info`, add:

```cpp
    server->Post("/motion", [manager](const httplib::Request& request,
                                     httplib::Response& response) {
        if (!request.has_param("motion_id")) {
            SetJsonResponse(&response, 400,
                            JsonResponseBuilder::BuildBadRequestResponse(
                                "missing required parameter 'motion_id'"));
            return;
        }
        const std::string motion_id =
            TrimCopy(request.get_param_value("motion_id"));

        const auto result = manager->InvokeMotion(motion_id);
        const int http_status = (result.code == 0)   ? 200
                              : (result.code == 400) ? 400
                              :                         502;
        SetJsonResponse(&response, http_status,
                        JsonResponseBuilder::BuildMotionResponse(result));
    });
```

No new includes needed — all referenced symbols are already in scope.

- [ ] **Step 2: Build**

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select robot_switch_server
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
git add src/robot_switch_server/src/infra/http_server_runner_httplib.cpp
git commit -m "feat(robot_switch_server): register POST /motion HTTP route"
```

---

### Task 6: Add `echo` + `fail_motion` services and motion declarations to `adapter_fake`

**Files:**
- Modify: `src/adapter_fake/src/adapter_fake_node.cpp`

- [ ] **Step 1: Add service members**

In `adapter_fake_node.cpp`, find the class's private members (near `sigterm_timer_` and `exit_timer_`). Add:

```cpp
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr echo_srv_;
    rclcpp::Service<std_srvs::srv::Trigger>::SharedPtr fail_motion_srv_;
```

- [ ] **Step 2: Register services in `RegisterExtensions`**

At the end of the existing `RegisterExtensions` method body (after the `exit_immediately` timer block), add:

```cpp
    const std::string prefix = std::string("/") + get_name() + "/";

    echo_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "echo",
        [](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
           std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
            response->success = true;
            response->message = "echo";
        });

    fail_motion_srv_ = create_service<std_srvs::srv::Trigger>(
        prefix + "fail_motion",
        [](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
           std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
            response->success = false;
            response->message = "forced failure";
        });
```

Both services respond unconditionally (no `connected_` gate) — they're test motions and the integration walk always runs `/start` first anyway.

- [ ] **Step 3: Declare the motion set in `OnSystemInfo`**

Replace the existing `OnSystemInfo` method body with:

```cpp
void OnSystemInfo(TriggerResponse response) override {
    robot_adapter_interfaces::SystemInfoBuilder system_info;
    system_info.SetDetailsJson("{\"adapter\":\"fake\",\"mode\":\"" +
                               behavior_mode_ + "\",\"connected\":" +
                               std::string(connected_ ? "true" : "false") + "}");
    system_info.SetMotions({
        {"echo", "echo", "Test success path"},
        {"fail_motion", "fail_motion", "Test failure path"},
    });

    response->success = true;
    response->message = system_info.Build();
}
```

- [ ] **Step 4: Build**

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select adapter_fake
```

Expected: success.

- [ ] **Step 5: Commit**

```bash
git add src/adapter_fake/src/adapter_fake_node.cpp
git commit -m "feat(adapter_fake): add echo/fail_motion services and declare motion set"
```

---

### Task 7: Integration walk on `adapter_fake`

This task **does not modify source code** — it's the live verification that Tasks 1–6 compose correctly end-to-end.

**Files:** none.

- [ ] **Step 1: Temporarily enable `fake` in the switch server config**

The default `server.yaml` enables only `go2` and `lynx`. For this walk, either edit `src/robot_switch_server/config/server.yaml` to add `"fake"` to `enabled_adapter_types`, **or** supply an override config via the launch arg. Simplest: in-place edit, reverted at the end.

Edit `src/robot_switch_server/config/server.yaml` to:

```yaml
robot_switch_server:
    ros__parameters:
        http_listen_address: "0.0.0.0:9098"
        service_wait_ms: 500
        call_timeout_ms: 2000
        adapter_connect_timeout_ms: 8000

        enabled_adapter_types:
            - "go2"
            - "lynx"
            - "fake"
```

Rebuild to install the config:

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select robot_switch_server
```

- [ ] **Step 2: Launch the stack**

```bash
source install/setup.bash
ros2 launch robot_switch_server robot_switch_system.launch.py
```

Leave this running in a terminal. From a second terminal:

- [ ] **Step 3: Run the verification curl sequence**

Each command must produce the expected shape. Substantial deviations are a failure — fix and re-run from Step 2.

```bash
# Start fake adapter
curl -s -X POST 'http://localhost:9098/start?adapter_type=fake'
# Expect: {"code":0,"msg":"adapter started and connected","data":{...}}

# Verify motions appear in system_info
curl -s 'http://localhost:9098/system_info' | python3 -m json.tool
# Expect: data.system_info.motions is an array containing
#   {"id":"echo","service_suffix":"echo","description":"Test success path"}
#   {"id":"fail_motion","service_suffix":"fail_motion","description":"Test failure path"}

# Success path
curl -s -X POST 'http://localhost:9098/motion?motion_id=echo'
# Expect: HTTP 200, {"code":0,"msg":"success","data":{"motion_id":"echo","detail":"echo"}}

# Adapter-rejected path
curl -s -X POST 'http://localhost:9098/motion?motion_id=fail_motion' -o /tmp/resp -w '%{http_code}\n'
cat /tmp/resp
# Expect: 502 status, {"code":502,"msg":"adapter rejected motion","data":{"motion_id":"fail_motion","detail":"forced failure"}}

# Unknown motion id
curl -s -X POST 'http://localhost:9098/motion?motion_id=nope' -o /tmp/resp -w '%{http_code}\n'
cat /tmp/resp
# Expect: 400 status, {"code":400,"msg":"unknown motion_id 'nope'","data":null}

# Invalid id (contains '/')
curl -s -X POST 'http://localhost:9098/motion?motion_id=a/b' -o /tmp/resp -w '%{http_code}\n'
cat /tmp/resp
# Expect: 400 status, {"code":400,"msg":"invalid motion_id","data":null}

# Missing param
curl -s -X POST 'http://localhost:9098/motion' -o /tmp/resp -w '%{http_code}\n'
cat /tmp/resp
# Expect: 400 status, {"code":400,"msg":"missing required parameter 'motion_id'","data":null}

# Stop, then try motion (adapter gone)
curl -s -X POST 'http://localhost:9098/stop'
curl -s -X POST 'http://localhost:9098/motion?motion_id=echo' -o /tmp/resp -w '%{http_code}\n'
cat /tmp/resp
# Expect: 400 status, {"code":400,"msg":"no adapter running","data":null}
```

If any command fails its expectation, debug before proceeding. Common failure points:
- `motions` missing from `/system_info`: `SystemInfoBuilder::Build()` didn't emit the key — revisit Task 1 Step 2.
- `echo` returns 502 "unknown motion": motion cache wasn't populated — revisit Task 3 Step 3 (Start's post-connect SystemInfo call).
- `fail_motion` returns 200 instead of 502: the `success=false` branch of `InvokeMotion` is wrong — revisit Task 3 Step 4 (the `rpc.reachable` vs `rpc.ok` logic).

- [ ] **Step 4: Revert the config and stop the launcher**

Stop the launcher (`Ctrl-C`). Revert `server.yaml`:

```bash
git checkout -- src/robot_switch_server/config/server.yaml
```

- [ ] **Step 5: Commit intentionally skipped**

No source-level changes to commit here. This task's output is confidence that the feature works end-to-end on fake.

---

### Task 8: Declare motion set in `adapter_go2`

**Files:**
- Modify: `src/adapter_go2/src/go2_adapter_node.cpp`

- [ ] **Step 1: Add `SetMotions` call in `OnSystemInfo`**

In `go2_adapter_node.cpp`, locate the `OnSystemInfo` method. Near the end, just before the line `system_info.SetDetailsJson(data.dump());`, insert:

```cpp
    system_info.SetMotions({
        {"stand", "stand", "Recovery stand"},
        {"stop", "stop", "Halt in place"},
        {"sit", "stop_and_sit", "Stop then sit down"},
        {"emergency_stop", "emergency_stop", "Damp all joints"},
    });
```

No new services — all four suffixes are already registered in the existing `RegisterExtensions`.

- [ ] **Step 2: Build**

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select adapter_go2
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
git add src/adapter_go2/src/go2_adapter_node.cpp
git commit -m "feat(adapter_go2): declare motion set in OnSystemInfo"
```

---

### Task 9: Declare motion set in `adapter_lynx`

**Files:**
- Modify: `src/adapter_lynx/src/lynx_adapter_node.cpp`

- [ ] **Step 1: Add `SetMotions` call in `OnSystemInfo`**

In `lynx_adapter_node.cpp`, locate the `OnSystemInfo` method. Just before the existing `system_info.SetDetailsJson(j.dump());` line, insert:

```cpp
    system_info.SetMotions({
        {"mode_regular",  "mode/regular",  "Switch to regular locomotion mode"},
        {"gait_walk",     "gait/walk",     "Switch gait to walk"},
        {"gait_trot",     "gait/trot",     "Switch gait to trot"},
        {"motion_normal", "motion/normal", "Normal motion state"},
        {"motion_agile",  "motion/agile",  "Agile motion state"},
        {"lights_on",     "lights/on",     "Turn lights on"},
        {"lights_off",    "lights/off",    "Turn lights off"},
        {"charge_start",  "charge/start",  "Start auto-charge"},
        {"charge_stop",   "charge/stop",   "Stop auto-charge"},
        {"sleep_enter",   "sleep/enter",   "Enter sleep mode"},
        {"sleep_exit",    "sleep/exit",    "Exit sleep mode"},
        {"sleep_query",   "sleep/query",   "Query sleep status"},
    });
```

All 12 suffixes are already registered in `RegisterExtensions`. Underscore ids with slash-bearing suffixes are deliberate (id regex forbids slashes).

- [ ] **Step 2: Build**

```bash
source /opt/ros/humble/setup.bash && \
  colcon build --packages-select adapter_lynx
```

Expected: success.

- [ ] **Step 3: Commit**

```bash
git add src/adapter_lynx/src/lynx_adapter_node.cpp
git commit -m "feat(adapter_lynx): declare motion set in OnSystemInfo"
```

---

## Final verification

After all 9 tasks, one full workspace build to confirm nothing cross-package drifted:

```bash
source /opt/ros/humble/setup.bash && \
  colcon build
```

Expected: success across all six packages.

The real-hardware smoke (one motion on go2, one on lynx) is out of scope for this plan — it's a post-merge, human-driven step as noted in the spec.
