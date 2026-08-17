# Motion Display Name Design

**Date**: 2026-07-29
**Status**: Approved

## Overview

Add a `display_name` field to `MotionDescriptor` carrying a short Chinese label (2–4 characters) for each discrete motion. The frontend renders `display_name` directly as button text instead of falling back to the raw `id` (`gait_agile_stairs`) or the English `description` (`"Prepare usage mode/RL and switch to agile stairs gait"`), neither of which is usable in a user-facing control.

The field is populated adapter-side, travels through the existing `/system_info` passthrough unchanged in shape, and is always non-empty by the time a client sees it.

## Goals

1. Give every declared motion a short, human-readable Chinese label the frontend can render without any client-side fallback logic.
2. Guarantee the field is always present and non-empty, so the frontend never renders a blank button.
3. Keep the change additive: adapters that don't set it, in-tree or out-of-tree, keep working with correct semantics.
4. Keep `id` (`[A-Za-z0-9_]+`, dispatch key) and `description` (long English text, developer-facing) unchanged in meaning.

## Non-Goals

- No i18n / multi-locale support. One Chinese label per motion; no locale negotiation, no English variant of `display_name`.
- No runtime/YAML configuration of labels — hardcoded in adapter C++ alongside the existing `description`, per the same pattern.
- No switch-server-side label mapping table. The switch server must not learn about specific motion ids; that would break the "server does not hardcode the motion set" property from the motion-control API design.
- No new motions. In particular the Lynx extension services not currently declared as motions (`lights/*`, `charge/*`, `sleep/*`, `gait/walk`, `gait/trot`, `motion/normal`, `motion/agile`) stay undeclared.
- No changes to `description` text, `id` values, `service_suffix` values, or the `/motion` dispatch path.
- No change to the `POST /motion` response shape.

## Contract

### Struct

```cpp
struct MotionDescriptor {
    std::string id;              // [A-Za-z0-9_]+, non-empty
    std::string service_suffix;  // appended to adapter service_prefix
    std::string description;     // optional long UX label (English); "" if none
    std::string display_name;    // short zh label for frontend; "" -> falls back to id
};
```

**`display_name` is appended at the end, not inserted before `description`.** This is deliberate and load-bearing.

All 16 in-tree call sites, plus out-of-tree ones, construct descriptors by *positional* aggregate initialization:

```cpp
{"stand_up", "stand_up", "Recover to standing posture"},
```

Inserting a new member in the middle keeps such three-element initializers compiling, but silently reinterprets the third argument as `display_name` and leaves `description` empty. Branch `feat/adapter-g1-velocity` carries three such initializers in `src/adapter_g1/include/adapter_g1/g1_adapter_contract.hpp`; that package's `CMakeLists.txt` never defines or applies `ROBOT_SPORT_WARN_OPTS` — `adapter_g1` is the only adapter package in the repo outside the `-Wall -Wextra -Wpedantic` baseline — so merging it after this change lands is silent and CI stays green, not a loud `-Werror` failure. G1's three motions fall through to the fallback below and surface `stand_up` / `stop` / `emergency_stop` as button captions in a Chinese UI — degraded, but not corrupt. That silent-but-safe outcome is exactly why appending at the end (rather than inserting before `description`) still pays off: it just can't lean on a compiler error as a safety net. The follow-up on `feat/adapter-g1-velocity` needs two things: the three `display_name` labels, and adding `ROBOT_SPORT_WARN_OPTS` to that package's targets so it rejoins the warning gate.

Appending instead makes stale three-element initializers keep their original meaning; `display_name` is empty and falls back to `id`. Degraded, visible, and correct.

There is no JSON-side cost: `nlohmann::json` object key order is independent of struct member order, and JSON object key order is not meaningful to consumers.

### Fallback rule

`display_name` empty → serialize `id` in its place. The emitted JSON's `display_name` is therefore always non-empty for any motion that survives validation.

### Validation

`display_name` does **not** participate in the drop rules. A motion is dropped only for an invalid `id`, an empty `service_suffix`, or a duplicate `id` — unchanged from today. A missing label degrades that one field; it never removes a motion from the list.

No charset validation on `display_name`: it is UTF-8 Chinese text, so the `IsValidMotionId` ASCII rule must not be applied to it.

## Where the fallback lives

**In `SystemInfoBuilder::SetMotions` (`robot_adapter_interfaces/src/system_info.cpp`), i.e. inside the adapter process.**

`AdapterRuntimeManager::GetSystemInfo` (`adapter_runtime_manager.cpp:499`) returns the adapter's raw payload string, and `JsonResponseBuilder::BuildSystemInfoResponse` splices it in via `AddRaw("system_info", payload)` without reserializing. The switch server therefore cannot alter the JSON the frontend receives — the fallback must be applied where that JSON is built.

`ParseMotionsFromSystemInfo` (`adapter_runtime_manager.cpp:50`) applies the same fallback when populating the cached motion map. That map is not what `/system_info` returns, so this is defense in depth for adapters that hand-roll their `/system_info` JSON instead of using `SystemInfoBuilder`, and it keeps the cached `MotionDescriptor` consistent with what clients see.

## Data flow

```
adapter_<type>_node::OnSystemInfo
  SystemInfoBuilder::SetMotions({... , display_name})
    │  drop invalid id / empty service_suffix / duplicate id
    │  display_name.empty() -> display_name = id
    ▼
  SystemInfoBuilder::Build()  ->  {"motions":[{id, service_suffix, description, display_name}, ...], ...}
    │  std_srvs/Trigger response.message
    ▼
robot_switch_server
  ├─ GET /system_info : raw payload passed through verbatim  -> frontend reads display_name
  └─ POST /start      : ParseMotionsFromSystemInfo -> cached map (same fallback applied)
```

## Changes by package

### 1. `robot_adapter_interfaces`

- `include/robot_adapter_interfaces/system_info.hpp` — add `display_name` as the last member of `MotionDescriptor`, with a comment stating the append-only rule and why.
- `src/system_info.cpp`
  - `ToMotionsJson` — emit `display_name`.
  - `SetMotions` — after the existing drop checks pass, apply `if (m.display_name.empty()) m.display_name = m.id;`.
- `CMakeLists.txt` — add a gtest target (see Testing).
- `ADAPTER_DEVELOPER_GUIDE.md` — document the field, the fallback, and the append-at-end rule for future adapter authors.

### 2. `robot_switch_server`

- `src/core/adapter_runtime_manager.cpp` — `ParseMotionsFromSystemInfo` reads `display_name` when present and applies the same fallback after id/suffix/duplicate validation.

No change to `json_response_builder.cpp`, the `/motion` route, or `InvokeMotionResult`.

### 3. `adapter_lynx`

`src/lynx_adapter_node.cpp:283` — add the fourth argument to all 10 descriptors.

### 4. `adapter_go2`

`src/go2_adapter_node.cpp:478` — add the fourth argument to all 4 descriptors.

### 5. `adapter_fake`

`src/adapter_fake_node.cpp:161` — add the fourth argument to both descriptors.

### 6. Docs

`docs/apis/Robot Sport后端接口文档.md`
- `GET /system_info` success example: add `display_name` to the `motions[]` entries.
- Notes section: document that `display_name` is non-empty for any adapter built on `SystemInfoBuilder` (all in-tree adapters), that the guarantee lives in the adapter process rather than in the switch server — which passes the payload through without revalidating — and that a frontend should therefore keep a one-line fallback to `id` for third-party adapters that hand-roll their `/system_info` JSON.
- `POST /motion` section: note that `motion_id` still comes from `motions[*].id` — `display_name` is display-only and must never be sent as `motion_id`.
- "当前已实现 Motion 集" table: add a `display_name` column.

## Display name table

| adapter | id | display_name |
|---|---|---|
| lynx | `mode_regular` | 常规模式 |
| lynx | `mode_navigation` | 导航模式 |
| lynx | `stand_up` | 站立 |
| lynx | `soft_stop` | 软急停 |
| lynx | `sit_down` | 趴下 |
| lynx | `rl_control` | RL 控制 |
| lynx | `gait_standard_flat` | 标准平地 |
| lynx | `gait_standard_stairs` | 标准爬楼 |
| lynx | `gait_agile_flat` | 敏捷平地 |
| lynx | `gait_agile_stairs` | 敏捷爬楼 |
| go2 | `stand_up` | 站立 |
| go2 | `stop` | 停止 |
| go2 | `sit_down` | 趴下 |
| go2 | `emergency_stop` | 急停 |
| fake | `echo` | 测试成功 |
| fake | `fail_motion` | 测试失败 |

`rl_control` keeps the "RL" jargon because it names a real body control state and is the prerequisite for all four gait motions; no plain-language rewrite preserves that meaning.

Duplicate labels across adapters (`站立`, `趴下`) are intentional — only one adapter is running at a time, so a client never sees both sets.

## Error handling

No new error paths. The fallback is total: every input maps to a valid output, so `SetMotions` and `ParseMotionsFromSystemInfo` gain no new failure modes, no new log lines, and no new HTTP status codes.

Existing drop-with-warning behavior for invalid `id` / empty `service_suffix` / duplicate `id` is unchanged.

## Testing

`robot_adapter_interfaces` currently has no gtest target. Add one following the established pattern in `adapter_lynx/CMakeLists.txt:51` (`ament_add_gtest`, `cxx_std_17`, `${ROBOT_SPORT_WARN_OPTS}`, `AMENT_LINT_AUTO_FILE_EXCLUDE "test/*"` so cppcheck does not scan test sources).

`test/test_system_info.cpp` covers:

1. **Label emitted** — a descriptor with a `display_name` serializes it verbatim in `motions[]`.
2. **Empty label falls back to id** — `{"lights_on", "lights/on", "Turn on lights", ""}` serializes `display_name == "lights_on"`, and `description` remains `"Turn on lights"` (this test is what would have caught the mid-struct-insertion mistake).
3. **Three-element initializer stays correct** — `{"a", "b", "desc"}` yields `description == "desc"` and `display_name == "a"`. Pins the append-at-end contract against future reordering.
4. **Drop rules unaffected by the label** — an invalid `id` or empty `service_suffix` is still dropped even when `display_name` is set; a valid motion with an empty `display_name` is still kept.
5. **`motions` key absent when `SetMotions` was never called** — pins the existing "byte-identical to pre-patch builds" property.

CI already runs `colcon build` with `-DROBOT_SPORT_WERROR=ON` and `colcon test` with cppcheck, so the new target enters the gate automatically.

Manual verification: start `adapter_fake` and `adapter_lynx`, `curl http://127.0.0.1:9098/system_info`, confirm every `motions[]` entry has a non-empty `display_name`.

## Out of scope / future work

- Declaring the remaining Lynx extension services as motions.
- Grouping / ordering hints for the frontend (e.g. a `category` field to cluster gait motions).
- Moving labels to YAML so they can be retuned without a rebuild — reconsider only if label churn becomes frequent.
- English `display_name` or a locale-keyed label map.
