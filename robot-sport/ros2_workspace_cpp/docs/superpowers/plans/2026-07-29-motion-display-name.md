# Motion Display Name Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a short Chinese `display_name` to every discrete motion declared by an adapter, so the frontend can render motion buttons directly instead of showing `gait_agile_stairs` or an English sentence.

**Architecture:** `MotionDescriptor` gains a fourth member, `display_name`, appended **last**. `SystemInfoBuilder::SetMotions` fills it with `id` when the adapter left it empty, so the JSON reaching the frontend is always non-empty. `GET /system_info` passes the adapter's raw payload through verbatim, so this is the only place the fallback can be applied for client-visible data; `robot_switch_server`'s motion-cache parser applies the same fallback as defense in depth.

**Tech Stack:** C++17, ROS2 Humble, colcon/ament, nlohmann_json, GoogleTest via `ament_add_gtest`.

**Spec:** `docs/superpowers/specs/2026-07-29-motion-display-name-design.md`

## Global Constraints

- All commands run from the colcon workspace root: `/home/zhangyuhan/workspace/production/v2/robot-sport/ros2_workspace_cpp`.
- `source /opt/ros/humble/setup.bash` before any colcon command.
- C++17. Every production target compiles with `-Wall -Wextra -Wpedantic`; CI adds `-Werror` via `-DROBOT_SPORT_WERROR=ON`.
- **`-Wextra` implies `-Wmissing-field-initializers`.** Once `MotionDescriptor` has four members, any three-element aggregate initializer warns — and hard-fails under CI's `-Werror`. Every in-tree call site must be updated in the same commit that adds the field, or the strict build goes red. This is verified in Task 1 Step 8.
- `display_name` must be the **last** member of `MotionDescriptor`. All call sites use positional aggregate initialization; moving it before `description` would silently reinterpret existing three-element initializers.
- `display_name` never participates in the drop rules. A motion is dropped only for an invalid `id`, an empty `service_suffix`, or a duplicate `id`.
- No charset validation on `display_name` — it is UTF-8 Chinese text. Never apply `IsValidMotionId` to it.
- Keep every `SetMotions` line within `.clang-format`'s `ColumnLimit: 80`. A descriptor that still fits on one line after the fourth field stays on one line; one that would overflow wraps after `service_suffix`, with `description` and `display_name` on a continuation line indented to the opening brace + 1. Mixed one-line and wrapped entries within the same block are expected and fine. (The Lynx block on this branch currently runs to 108 columns; this task brings it back under the limit rather than extending the overflow.)
- Exact `display_name` values are fixed by the spec's table. Do not invent, translate, or "improve" them.

---

### Task 1: `display_name` contract, fallback, tests, and all in-tree adapter labels

This is one commit because of the `-Werror` constraint above: adding the struct member without updating the three adapters leaves the workspace un-buildable in strict mode.

**Files:**
- Modify: `src/robot_adapter_interfaces/include/robot_adapter_interfaces/system_info.hpp:16-20`
- Modify: `src/robot_adapter_interfaces/src/system_info.cpp:45-55` (`ToMotionsJson`), `:85-110` (`SetMotions`)
- Modify: `src/robot_adapter_interfaces/CMakeLists.txt` (`if(BUILD_TESTING)` block, currently lint-only)
- Modify: `src/robot_adapter_interfaces/package.xml` (add `ament_cmake_gtest` test dep)
- Modify: `src/adapter_lynx/src/lynx_adapter_node.cpp:283-294`
- Modify: `src/adapter_go2/src/go2_adapter_node.cpp:478-483`
- Modify: `src/adapter_fake/src/adapter_fake_node.cpp:161-164`
- Create: `src/robot_adapter_interfaces/test/test_system_info.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `robot_adapter_interfaces::MotionDescriptor` with members, in order, `std::string id; std::string service_suffix; std::string description; std::string display_name;`. `SystemInfoBuilder::SetMotions(std::vector<MotionDescriptor>)` guarantees every surviving descriptor has a non-empty `display_name` and emits it under the JSON key `"display_name"`. Task 2 relies on that key name.

- [ ] **Step 1: Add the gtest scaffolding**

In `src/robot_adapter_interfaces/package.xml`, add the gtest test dependency above the existing lint deps:

```xml
  <test_depend>ament_cmake_gtest</test_depend>
  <test_depend>ament_lint_auto</test_depend>
  <test_depend>ament_lint_common</test_depend>
```

In `src/robot_adapter_interfaces/CMakeLists.txt`, replace the whole `if(BUILD_TESTING)` block with:

```cmake
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_lint_auto REQUIRED)

  # 直接编译 system_info.cpp 进测试目标(同 adapter_lynx 的 test_lynx_sdk_client 写法),
  # 避免链接整个依赖 rclcpp 的共享库——本测试只用到 SystemInfoBuilder + nlohmann。
  ament_add_gtest(test_system_info
    test/test_system_info.cpp
    src/system_info.cpp
  )
  target_compile_features(test_system_info PRIVATE cxx_std_17)
  target_compile_options(test_system_info PRIVATE ${ROBOT_SPORT_WARN_OPTS})
  target_include_directories(test_system_info PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(test_system_info
    nlohmann_json::nlohmann_json
  )

  # 只让 cppcheck 进门禁:copyright/cpplint 在此跳过;uncrustify/lint_cmake/xmllint 等风格项
  # 由 CI colcon test 的 -E 过滤(对存量代码必红、属风格债,暂不纳入门禁)。
  set(ament_cmake_copyright_FOUND TRUE)
  set(ament_cmake_cpplint_FOUND TRUE)
  # cppcheck 2.x 不会展开 GoogleTest 的 TEST 宏，会将第二个测试用例误报为
  # syntaxError。测试源仍由 ament_add_gtest 编译并运行；仅将其排除在 cppcheck
  # 的源码扫描之外，保留对生产 src/ 和 include/ 的静态分析。
  set(AMENT_LINT_AUTO_FILE_EXCLUDE "test/*")
  ament_lint_auto_find_test_dependencies()
endif()
```

- [ ] **Step 2: Write the failing tests**

Create `src/robot_adapter_interfaces/test/test_system_info.cpp`:

```cpp
#include "robot_adapter_interfaces/system_info.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace {

using robot_adapter_interfaces::MotionDescriptor;
using robot_adapter_interfaces::SystemInfoBuilder;

// Runs a descriptor list through the builder and hands back the serialized
// `motions` array, i.e. exactly what a frontend would receive.
nlohmann::json BuildMotions(std::vector<MotionDescriptor> motions) {
    SystemInfoBuilder builder;
    builder.SetMotions(std::move(motions));
    return nlohmann::json::parse(builder.Build()).at("motions");
}

}  // namespace

TEST(SystemInfoMotions, DisplayNameIsEmitted) {
    const auto motions = BuildMotions({
        {"stand_up", "stand_up", "Recover to standing posture", "站立"},
    });
    ASSERT_EQ(motions.size(), 1u);
    EXPECT_EQ(motions[0].at("id").get<std::string>(), "stand_up");
    EXPECT_EQ(motions[0].at("service_suffix").get<std::string>(), "stand_up");
    EXPECT_EQ(motions[0].at("description").get<std::string>(),
              "Recover to standing posture");
    EXPECT_EQ(motions[0].at("display_name").get<std::string>(), "站立");
}

TEST(SystemInfoMotions, EmptyDisplayNameFallsBackToId) {
    const auto motions = BuildMotions({
        {"lights_on", "lights/on", "Turn on lights", ""},
    });
    ASSERT_EQ(motions.size(), 1u);
    EXPECT_EQ(motions[0].at("display_name").get<std::string>(), "lights_on");
    // The fallback must not disturb the neighbouring field.
    EXPECT_EQ(motions[0].at("description").get<std::string>(),
              "Turn on lights");
}

TEST(SystemInfoMotions, ThreeElementInitializerKeepsDescription) {
    // display_name must stay the LAST member of MotionDescriptor. If someone
    // reorders it before `description`, this positional initializer silently
    // routes the English text into display_name — this test is what catches
    // that. -Wmissing-field-initializers is suppressed on purpose: omitting
    // the field IS the scenario under test, and CI compiles with -Werror.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
    const std::vector<MotionDescriptor> legacy = {
        {"stand_up", "stand_up", "Recover to standing posture"},
    };
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

    const auto motions = BuildMotions(legacy);
    ASSERT_EQ(motions.size(), 1u);
    EXPECT_EQ(motions[0].at("description").get<std::string>(),
              "Recover to standing posture");
    EXPECT_EQ(motions[0].at("display_name").get<std::string>(), "stand_up");
}

TEST(SystemInfoMotions, DropRulesIgnoreDisplayName) {
    const auto motions = BuildMotions({
        {"bad id", "whatever", "invalid charset in id", "非法"},
        {"no_suffix", "", "empty service_suffix", "无后缀"},
        {"dup", "dup", "first wins", "重复一"},
        {"dup", "dup2", "second dropped", "重复二"},
        {"kept", "kept", "no label supplied", ""},
    });
    // A present display_name never rescues an invalid motion; an absent one
    // never removes a valid motion. SetMotions preserves declaration order.
    ASSERT_EQ(motions.size(), 2u);
    EXPECT_EQ(motions[0].at("id").get<std::string>(), "dup");
    EXPECT_EQ(motions[0].at("display_name").get<std::string>(), "重复一");
    EXPECT_EQ(motions[1].at("id").get<std::string>(), "kept");
    EXPECT_EQ(motions[1].at("display_name").get<std::string>(), "kept");
}

TEST(SystemInfoMotions, MotionsKeyAbsentWhenNeverDeclared) {
    SystemInfoBuilder builder;
    builder.SetBattery(87);
    const auto parsed = nlohmann::json::parse(builder.Build());
    EXPECT_FALSE(parsed.contains("motions"));
}
```

- [ ] **Step 3: Run the tests to verify they fail**

```bash
source /opt/ros/humble/setup.bash
colcon build --packages-select robot_adapter_interfaces \
  --cmake-args -DBUILD_TESTING=ON
```

Expected: **compile error**, because `MotionDescriptor` has only three members today:

```
error: too many initializers for 'robot_adapter_interfaces::MotionDescriptor'
```

That compile failure is the red state. Do not proceed until you have seen it.

- [ ] **Step 4: Add `display_name` to the struct**

In `src/robot_adapter_interfaces/include/robot_adapter_interfaces/system_info.hpp`, replace the `MotionDescriptor` definition:

```cpp
struct MotionDescriptor {
    std::string id;              // [A-Za-z0-9_]+, non-empty
    std::string service_suffix;  // appended to adapter service_prefix (e.g. "stop_and_sit")
    std::string description;     // optional UX label; "" if none
    // 前端直接渲染的中文短名(2-4 字)。留空时 SetMotions 会回填 id,调用方永远拿不到空串。
    //
    // 必须保持在最后一个成员。所有调用点都用位置聚合初始化
    // ({"id", "suffix", "desc", "label"});把它挪到 description 之前不会编译失败,
    // 而是把英文长句静默塞进 display_name。
    std::string display_name;
};
```

- [ ] **Step 5: Emit the field and apply the fallback**

In `src/robot_adapter_interfaces/src/system_info.cpp`, in `ToMotionsJson`, add the key:

```cpp
nlohmann::json ToMotionsJson(const std::vector<MotionDescriptor>& motions) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : motions) {
        arr.push_back({
            {"id", m.id},
            {"service_suffix", m.service_suffix},
            {"description", m.description},
            {"display_name", m.display_name},
        });
    }
    return arr;
}
```

In the same file, in `SetMotions`, insert the fallback after the duplicate-id check and immediately before the `push_back`, so it applies only to motions that survived validation:

```cpp
        if (!seen.insert(m.id).second) {
            std::cerr << "[SystemInfoBuilder] dropping motion '" << m.id
                      << "': duplicate id" << std::endl;
            continue;
        }
        // 短名缺失只降级,不丢动作:前端拿到 id 总好过拿到空按钮。
        if (m.display_name.empty()) {
            m.display_name = m.id;
        }
        valid.push_back(std::move(m));
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
source /opt/ros/humble/setup.bash
colcon build --packages-select robot_adapter_interfaces \
  --cmake-args -DBUILD_TESTING=ON -DROBOT_SPORT_WERROR=ON
colcon test --packages-select robot_adapter_interfaces \
  --ctest-args -R test_system_info --event-handlers console_direct+
colcon test-result --verbose
```

Expected: build succeeds with `-Werror` on, and all 5 tests pass.

- [ ] **Step 7: Add the Chinese labels to all three adapters**

`src/adapter_lynx/src/lynx_adapter_node.cpp` — replace the `SetMotions` block at line 283:

```cpp
    system_info.SetMotions({
        {"mode_regular", "mode/regular",
         "Use normalized Command=21 axis control", "常规模式"},
        {"mode_navigation", "mode/navigation",
         "Use absolute Command=25 velocity control", "导航模式"},
        {"stand_up", "stand_up", "Switch to standing posture", "站立"},
        {"soft_stop", "soft_stop", "Switch to soft stop posture", "软急停"},
        {"sit_down", "sit_down", "Switch to prone posture", "趴下"},
        {"rl_control", "rl_control", "Enter RL control state", "RL 控制"},
        {"gait_standard_flat", "gait/standard_flat",
         "Prepare RL and switch to standard flat gait", "标准平地"},
        {"gait_standard_stairs", "gait/standard_stairs",
         "Prepare RL and switch to standard stairs gait", "标准爬楼"},
        {"gait_agile_flat", "gait/agile_flat",
         "Prepare usage mode/RL and switch to agile flat gait", "敏捷平地"},
        {"gait_agile_stairs", "gait/agile_stairs",
         "Prepare usage mode/RL and switch to agile stairs gait", "敏捷爬楼"},
    });
```

The four short entries stay on one line because they fit within 80 columns; the six long ones wrap. Do not "normalize" this into a single uniform shape.

`src/adapter_go2/src/go2_adapter_node.cpp` — replace the `SetMotions` block at line 478:

```cpp
    system_info.SetMotions({
        {"stand_up", "stand_up", "Recover to standing posture", "站立"},
        {"stop", "stop", "Halt in place", "停止"},
        {"sit_down", "sit_down", "Stop motion then stand down", "趴下"},
        {"emergency_stop", "emergency_stop", "Damp all joints", "急停"},
    });
```

`src/adapter_fake/src/adapter_fake_node.cpp` — replace the `SetMotions` block at line 161:

```cpp
        system_info.SetMotions({
            {"echo", "echo", "Test success path", "测试成功"},
            {"fail_motion", "fail_motion", "Test failure path", "测试失败"},
        });
```

- [ ] **Step 8: Strict-build the whole workspace**

```bash
source /opt/ros/humble/setup.bash
colcon build --packages-select robot_adapter_interfaces adapter_fake adapter_lynx adapter_go2 robot_switch_server \
  --cmake-args -DBUILD_TESTING=ON -DROBOT_SPORT_WERROR=ON
```

Expected: green. Any `-Wmissing-field-initializers` error here means a `SetMotions` call site was missed — fix it rather than relaxing the flag.

If `adapter_go2` fails because the `unitree_sdk2` submodule is unavailable, drop `adapter_go2` from `--packages-select` and note it; CI skips that package under the same condition (see `.gitea/workflows/ci.yml` line 12-14). The go2 source edit still stands.

- [ ] **Step 9: Verify end to end against a live adapter**

```bash
source /opt/ros/humble/setup.bash
source install/setup.bash
ros2 run adapter_fake adapter_fake_node &
sleep 2
ros2 service call /adapter_fake/system_info std_srvs/srv/Trigger
kill %1
```

Expected: the `message` field contains a `motions` array where both entries carry `"display_name": "测试成功"` and `"display_name": "测试失败"`.

- [ ] **Step 10: Commit**

Stage the files explicitly — `ADAPTER_DEVELOPER_GUIDE.md` also lives under
`src/robot_adapter_interfaces/` but belongs to Task 3.

```bash
git add src/robot_adapter_interfaces/include/robot_adapter_interfaces/system_info.hpp \
        src/robot_adapter_interfaces/src/system_info.cpp \
        src/robot_adapter_interfaces/test/test_system_info.cpp \
        src/robot_adapter_interfaces/CMakeLists.txt \
        src/robot_adapter_interfaces/package.xml \
        src/adapter_lynx/src/lynx_adapter_node.cpp \
        src/adapter_go2/src/go2_adapter_node.cpp \
        src/adapter_fake/src/adapter_fake_node.cpp
git commit -m "feat(robot_adapter_interfaces): add display_name to MotionDescriptor

Frontend renders motion buttons straight from display_name instead of
falling back to the raw id (gait_agile_stairs) or the English description.

- display_name is the LAST struct member: every call site uses positional
  aggregate init, so inserting it before description would silently move
  the English text into the label with no compiler diagnostic.
- SetMotions backfills id when the label is empty, so the JSON reaching a
  client is always non-empty. It never affects the drop rules.
- Adds the first gtest target for robot_adapter_interfaces, pinning the
  fallback and the field ordering.
- Labels all 16 in-tree motions (lynx 10, go2 4, fake 2) in the same
  commit: -Wextra implies -Wmissing-field-initializers, so a stale
  three-element initializer breaks the CI -Werror build."
```

---

### Task 2: Apply the same fallback in the switch server motion cache

`GET /system_info` passes the adapter payload through untouched, so this does not change what the frontend sees. It keeps the cached `MotionDescriptor` consistent with the wire format for adapters that hand-roll their `/system_info` JSON instead of using `SystemInfoBuilder`.

**Files:**
- Modify: `src/robot_switch_server/src/core/adapter_runtime_manager.cpp:76-105` (`ParseMotionsFromSystemInfo`)

**Interfaces:**
- Consumes: `robot_adapter_interfaces::MotionDescriptor::display_name` and the JSON key `"display_name"`, both from Task 1.
- Produces: nothing new. `RunningAdapter::motions` values now carry a non-empty `display_name`.

- [ ] **Step 1: Parse the field**

In `ParseMotionsFromSystemInfo`, after the existing `description` block, add:

```cpp
        if (entry.contains("display_name") &&
            entry.at("display_name").is_string()) {
            desc.display_name = entry.at("display_name").get<std::string>();
        }
```

- [ ] **Step 2: Apply the fallback after validation**

Immediately before `map.emplace(desc.id, std::move(desc));`, add:

```cpp
        // 与 SystemInfoBuilder::SetMotions 保持同一条回落规则,兜住不用
        // SystemInfoBuilder、自己拼 /system_info JSON 的 adapter。
        if (desc.display_name.empty()) {
            desc.display_name = desc.id;
        }
        map.emplace(desc.id, std::move(desc));
```

- [ ] **Step 3: Strict-build**

```bash
source /opt/ros/humble/setup.bash
colcon build --packages-select robot_switch_server \
  --cmake-args -DBUILD_TESTING=ON -DROBOT_SPORT_WERROR=ON
colcon test --packages-select robot_switch_server --event-handlers console_direct+ \
  --ctest-args -E '(copyright|cpplint|flake8|lint_cmake|pep257|uncrustify|xmllint)'
colcon test-result --verbose
```

Expected: build green, cppcheck clean. `robot_switch_server` has no gtest target, so no unit tests run — the `colcon test` invocation is there to confirm cppcheck still passes on the edited file.

- [ ] **Step 4: Commit**

```bash
git add src/robot_switch_server/src/core/adapter_runtime_manager.cpp
git commit -m "feat(robot_switch_server): mirror display_name fallback in motion cache

Defense in depth for adapters that build their /system_info JSON by hand
instead of using SystemInfoBuilder. GET /system_info passes the adapter
payload through verbatim, so this does not alter what clients receive; it
keeps the cached MotionDescriptor consistent with the wire format."
```

---

### Task 3: Documentation

**Files:**
- Modify: `docs/apis/Robot Sport后端接口文档.md` (lines 338, 402-417, 496-517, 554-558)
- Modify: `src/robot_adapter_interfaces/ADAPTER_DEVELOPER_GUIDE.md` (section 2.3, lines 91-125)

**Interfaces:**
- Consumes: the final JSON shape produced by Task 1.
- Produces: nothing consumed by other tasks.

- [ ] **Step 1: Update the `GET /system_info` response example**

In `docs/apis/Robot Sport后端接口文档.md`, replace the `motions` array at lines 496-517:

```json
      "motions": [
        {
          "id": "stand_up",
          "service_suffix": "stand_up",
          "description": "Recover to standing posture",
          "display_name": "站立"
        },
        {
          "id": "stop",
          "service_suffix": "stop",
          "description": "Halt in place",
          "display_name": "停止"
        },
        {
          "id": "sit_down",
          "service_suffix": "sit_down",
          "description": "Stop motion then stand down",
          "display_name": "趴下"
        },
        {
          "id": "emergency_stop",
          "service_suffix": "emergency_stop",
          "description": "Damp all joints",
          "display_name": "急停"
        }
      ],
```

- [ ] **Step 2: Update the `/system_info` notes**

Replace the 说明 bullets at lines 554-558 with:

```markdown
- `system_info` 为适配器透传 JSON，对象顶层统一包含 `battery`、`motion`、`details`
- 若适配器声明了离散动作，还会额外包含 `motions` 数组
- `motion` 表示当前速度状态 `{x, y, yaw}`，不是离散动作名
- `motions[*].display_name` 是给用户看的中文短名。基于 `SystemInfoBuilder` 构建 `/system_info`（本仓库内所有适配器均如此）时该字段保证非空——适配器未显式提供时，`SystemInfoBuilder::SetMotions` 会自动回落成 `id`；但这个保证来自适配器进程内部，`robot_switch_server` 只是把适配器返回的原始 JSON 透传给前端，并不会重新校验或补全该字段。若接入未使用 `SystemInfoBuilder`、自行拼接 `/system_info` JSON 的第三方适配器，`display_name` 键可能整体缺失，前端仍应保留一行回落到 `id` 的兜底逻辑
- `motions[*].description` 是面向开发者的英文长描述，不适合直接展示给用户
- 调用 `/motion` 前，应先读取 `system_info.motions[*].id`
- 不同适配器返回的 `details` 内容不同，调用方应按对象动态解析
```

- [ ] **Step 3: Clarify the `motion_id` parameter**

The parameter table row at line 338 stays as it is. Add one blockquote directly under that table (i.e. after line 339, before the `#### 调用方式` heading):

```markdown
> `display_name` 是纯展示字段，不能拿来当 `motion_id` 传；派发只认 `id`。
```

- [ ] **Step 4: Add a `display_name` column to the implemented-motion table**

Replace lines 402-417:

```markdown
| 适配器 | motion_id | display_name | 说明 |
|---|---|---|---|
| `go2` | `stand_up` | 站立 | 站起，调用 GO2 `RecoveryStand()` |
| `go2` | `stop` | 停止 | 原地停止，调用 GO2 `StopMove()` |
| `go2` | `sit_down` | 趴下 | 趴下，先 `StopMove()` 再 `StandDown()` |
| `go2` | `emergency_stop` | 急停 | 紧急停，调用 GO2 `Damp()` |
| `lynx` | `stand_up` | 站立 | 站起，调用 Lynx `SetMotionState(1)` |
| `lynx` | `soft_stop` | 软急停 | 软急停，调用 Lynx `SetMotionState(2)` |
| `lynx` | `sit_down` | 趴下 | 趴下，调用 Lynx `SetMotionState(4)` |
| `lynx` | `mode_regular` | 常规模式 | 切换常规模式（`ControlUsageMode=0`），速度使用 `Command=21` 轴比例 |
| `lynx` | `mode_navigation` | 导航模式 | 切换导航模式（`ControlUsageMode=1`），速度使用 `Command=25` 绝对值 |
| `lynx` | `rl_control` | RL 控制 | 进入 RL 控制状态（`MotionState=17`） |
| `lynx` | `gait_standard_flat` | 标准平地 | 自动准备 RL 状态并切换标准平地步态，`GaitParam=0x1001` |
| `lynx` | `gait_standard_stairs` | 标准爬楼 | 自动准备 RL 状态并切换标准楼梯步态，`GaitParam=0x1003` |
| `lynx` | `gait_agile_flat` | 敏捷平地 | 必要时自动切导航模式、进入 RL，再切敏捷平地，`GaitParam=0x3002` |
| `lynx` | `gait_agile_stairs` | 敏捷爬楼 | 必要时自动切导航模式、进入 RL，再切敏捷楼梯，`GaitParam=0x3003` |
```

- [ ] **Step 5: Document the field for adapter authors**

`src/robot_adapter_interfaces/ADAPTER_DEVELOPER_GUIDE.md` currently documents only `battery` / `motion` / `details` in section 2.3 and never mentions `motions`. Add `motions` to the schema example at lines 91-109 so it reads:

```json
{
  "battery": 87,
  "motion": {
    "x": 0.02,
    "y": 0.0,
    "yaw": -0.1
  },
  "motions": [
    {
      "id": "stand_up",
      "service_suffix": "stand_up",
      "description": "Recover to standing posture",
      "display_name": "站立"
    }
  ],
  "details": {
    "vendor_model": "your_robot",
    "battery_pct": 87,
    "velocity": {
      "x": 0.02,
      "y": 0.0,
      "yaw": -0.1
    }
  }
}
```

Add a row to the field table at lines 113-117, after the `motion` row:

```markdown
| `motions` | `object[]` | 本 adapter 声明的离散动作集合，元素为 `{id, service_suffix, description, display_name}`。未调用 `SetMotions()` 时整个键不出现 |
```

Add these bullets to the 约束要求 list at lines 121-125:

```markdown
- `motions[*].id` 必须是非空的 `[A-Za-z0-9_]+`，`service_suffix` 不能为空；违反任一条的动作会被 `SetMotions()` 直接丢弃并打印告警。
- `motions[*].display_name` 是前端直接渲染的中文短名（2-4 字）。留空时 `SetMotions()` 会回填 `id`，所以调用方永远拿不到空串；但请不要依赖这个回落，新增动作时把短名填全。
- `MotionDescriptor` 的 `display_name` 必须保持在最后一个成员。所有调用点都用位置聚合初始化，把它挪到 `description` 之前不会编译失败，只会把英文长句静默塞进短名字段。
```

- [ ] **Step 6: Verify the JSON examples parse**

```bash
python3 - <<'PY'
import json, re, pathlib
for p in ["docs/apis/Robot Sport后端接口文档.md",
          "src/robot_adapter_interfaces/ADAPTER_DEVELOPER_GUIDE.md"]:
    text = pathlib.Path(p).read_text(encoding="utf-8")
    blocks = re.findall(r"```json\n(.*?)```", text, re.S)
    bad = 0
    for i, b in enumerate(blocks):
        try:
            json.loads(b)
        except json.JSONDecodeError as e:
            bad += 1
            print(f"{p} block {i}: {e}")
    print(f"{p}: {len(blocks)} json blocks, {bad} invalid")
PY
```

Expected: the invalid-block count is unchanged from before your edits — **not** `0 invalid`. Both files carry a pre-existing placeholder block that cannot parse as JSON on purpose: the `"data": { ... }` example under "2.3 统一响应结构" in the API doc, and the `"Type": <功能类型>` example under "11.3 协议说明" in the adapter guide. Run the script before making any edits in this task to get the baseline count, then compare after; a trailing comma or a missing brace in the examples you actually touched shows up as a change in that count. Do not "fix" the two known placeholders — they are intentional and out of scope.

- [ ] **Step 7: Commit**

```bash
git add "docs/apis/Robot Sport后端接口文档.md" \
        src/robot_adapter_interfaces/ADAPTER_DEVELOPER_GUIDE.md
git commit -m "docs: document motions[].display_name

- /system_info example and notes: display_name is non-empty for adapters
  built on SystemInfoBuilder, but the switch server passes the payload
  through without revalidating, so clients keep a one-line id fallback;
  description stays developer-facing.
- /motion: display_name is display-only and must never be sent as motion_id.
- Implemented-motion table gains a display_name column.
- Adapter developer guide documents the motions array for the first time,
  including the drop rules and the keep-display_name-last requirement."
```

---

## Notes for the reviewer

- The `feat/adapter-g1-velocity` branch carries three `MotionDescriptor` initializers in `src/adapter_g1/include/adapter_g1/g1_adapter_contract.hpp`. `adapter_g1`'s `CMakeLists.txt` never defines or applies `ROBOT_SPORT_WARN_OPTS` — it is the only adapter package in the repo outside the `-Wall -Wextra -Wpedantic` baseline — so after this change lands, that branch does **not** fail to compile: the merge is silent and CI stays green. G1's three motions fall through to the `SetMotions` fallback and surface `stand_up` / `stop` / `emergency_stop` as button captions in a Chinese UI instead of a proper label — degraded, but not corrupt, which is exactly why appending `display_name` at the end (rather than inserting it before `description`) still pays off even without a compiler backstop. The follow-up on `feat/adapter-g1-velocity` needs two things: the three `display_name` labels, and adding `ROBOT_SPORT_WARN_OPTS` to that package's targets so it rejoins the warning gate. Suggested labels when that branch is rebased: `stand_up` → 站立, `stop` → 停止, `emergency_stop` → 急停.
- This branch (`feat/motion-display-name`) is based on `fix/adapter-lynx-velocity-conversion`, not `main`. Seven of the ten Lynx labels are for motions that only exist on that branch, so it must merge first.
