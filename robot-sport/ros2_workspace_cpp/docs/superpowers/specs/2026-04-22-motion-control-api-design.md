# Motion Control API Design

**Date**: 2026-04-22
**Status**: Approved

## Overview

Add an HTTP motion-control API to `robot_switch_server` that dispatches discrete named actions (for example `stand_up`, `sit_down`, `stop`, `emergency_stop`) to whichever adapter is currently running. Each adapter declares its own motion set in `/system_info`; the user invokes a motion by its declared id via `POST /motion?motion_id=<id>`.

Continuous velocity control remains on `cmd_vel` via `remote_controller` and is unchanged by this work.

## Goals

1. Let adapter authors declare a typed motion set in `/system_info` without touching the switch server.
2. Give HTTP clients a single, stable endpoint (`POST /motion`) to trigger those motions.
3. Validate unknown/malformed motion ids at the switch server so callers get clean 400s instead of ROS-level errors.
4. Preserve strict separation between motion control (this API) and velocity control (`cmd_vel` / `remote_controller`).

## Non-Goals

- No parameterized motions (every motion is a `std_srvs/srv/Trigger` — no payload).
- No streaming / continuous velocity via `/motion`.
- No adapter-side process-lifecycle or state-machine changes.
- No runtime mutation of a declared motion set; a stop/start cycle is required to pick up changes.
- No changes to `AdapterNodeBase`'s 5 standard services or the `RegisterExtensions` hook.
- No new unit-test framework at the workspace level; coverage piggybacks on existing integration patterns.

## Architecture

```
HTTP client
     │  POST /motion?motion_id=<id>
     ▼
robot_switch_server
  • http_server_runner_httplib.cpp        (new /motion route)
  • AdapterRuntimeManager::InvokeMotion   (new method)
       │
       │ validates: adapter running, state=Running, id in cached set
       │
       ▼
  AdapterClient::CallTriggerByName(service_suffix, timeout)
       │  ROS2 service call on /adapter_<type>/<service_suffix>
       ▼
adapter_<type>_node
  • Existing extension services (no change) respond success/failure.

On POST /start (immediately after Connect succeeds):
  AdapterRuntimeManager fetches /system_info once, parses `motions` array,
  populates RunningAdapter::motions  (id → MotionDescriptor).
On POST /stop (or adapter crash): motions map is cleared.
```

## API Surface

### `POST /motion?motion_id=<id>`

Query param only; empty body. Matches the shape of the existing `/start?adapter_type=...` route.

**`motion_id` constraints:** non-empty, `[A-Za-z0-9_]+`, no spaces, dots, dashes, or slashes. Validated at the HTTP layer and re-validated in the manager as a second line of defense.

**Responses** (envelope `{code, msg, data}` via `JsonResponseBuilder`):

| Case | HTTP | `code` | `msg` | `data` |
|---|---|---|---|---|
| Success | 200 | 0 | `"success"` | `{motion_id, detail: <adapter msg>}` |
| Missing `motion_id` param | 400 | 400 | `"missing required parameter 'motion_id'"` | `null` |
| `motion_id` fails regex | 400 | 400 | `"invalid motion_id"` | `null` |
| No adapter running | 400 | 400 | `"no adapter running"` | `null` |
| Adapter state ≠ `Running` | 400 | 400 | `"adapter not ready (state=<State>)"` | `null` |
| Unknown motion id | 400 | 400 | `"unknown motion_id '<id>'"` | `null` |
| Adapter Trigger returns `success=false` | 502 | 502 | `"adapter rejected motion"` | `{motion_id, detail: <adapter msg>}` |
| RPC timeout / transport error | 502 | 502 | `"adapter call failed"` | `{motion_id, detail: <error>}` |

**400 vs 502 split**: 400 = caller-fixable (bad request, wrong state, unknown id). 502 = adapter-side fault the caller can't preempt. Monitors can distinguish user typos from robot misbehavior.

### `GET /system_info` (already exists)

Each running adapter's declared motion set is exposed here under a new top-level `motions` field. Callers can list-before-invoke; no separate `/motions` endpoint is added.

## Changes by Package

### 1. `robot_adapter_interfaces`

**`system_info.hpp`** — add a first-class motion set:

```cpp
struct MotionDescriptor {
    std::string id;              // e.g. "sit"  — [A-Za-z0-9_]+
    std::string service_suffix;  // e.g. "stop_and_sit" — appended to service_prefix
    std::string description;     // optional UX label; "" if none
};

class SystemInfoBuilder {
    // ... existing SetBattery / SetMotion / SetDetailsJson ...
    SystemInfoBuilder& SetMotions(std::vector<MotionDescriptor> motions);
};
```

**`system_info.cpp`** — emit `"motions": [...]` as a top-level key in `Build()`, alongside `battery`, `motion`, `details`. Rules:

- Omitted entirely when `SetMotions` was never called (no `"motions": null`). Keeps adapters that don't opt in byte-identical to today.
- Entry with empty `id`, empty `service_suffix`, or `id` failing `[A-Za-z0-9_]+` is dropped with a log line — one bad entry does not poison the rest. Matches the forgiving style of `ParseDetailsObject`.
- Duplicate `id` within the same `SetMotions` call: first occurrence wins, later ones are dropped with a log line.

**`AdapterClient`** — add one method, do not touch the existing five:

```cpp
// adapter_client.hpp
AdapterCallResult CallTriggerByName(const std::string& service_suffix,
                                    std::chrono::milliseconds timeout);
```

Implementation creates an `rclcpp::Client<std_srvs::srv::Trigger>` on the internal node for `service_prefix_ + "/" + service_suffix`, invokes via the existing `SingleThreadedExecutor`, and funnels through the same private `CallTrigger` helper used by the five fixed methods. Created clients are cached in `std::unordered_map<std::string, Client::SharedPtr>` on the `AdapterClient` instance so repeat invocations of the same motion reuse the handle.

### 2. `robot_switch_server`

**`AdapterRuntimeManager`** (`core/adapter_runtime_manager.{hpp,cpp}`):

- Extend `RunningAdapter` with `std::unordered_map<std::string, MotionDescriptor> motions`.
- In `Start()`, after the Connect RPC succeeds and the state transitions to `Running`, synchronously call the adapter's `/system_info`, parse the `motions` array, validate each entry (same rules as adapter-side, belt-and-braces), and populate `running_->motions`. An empty map is legal (adapter declared no motions).
- If `/system_info` fails or the payload is unparseable at this moment: log a warning and leave `motions` empty. Do **not** fail `Start()` — motion declaration is opt-in and non-critical to adapter health. A subsequent `POST /motion` will cleanly return `"unknown motion_id"`.
- In `Stop()` (and any crash-driven transition that clears `running_`): the map is cleared alongside the rest of `RunningAdapter`.
- New public method:

```cpp
struct InvokeMotionResult {
    OperationResult operation;   // reuses existing {code, msg, detail}
    std::string motion_id;
};
InvokeMotionResult InvokeMotion(const std::string& motion_id);
```

Dispatch, serialized under the same `std::mutex` that guards `running_`:

1. No `running_` → `code=400, msg="no adapter running"`.
2. State ≠ `Running` → `code=400, msg="adapter not ready (state=...)"`.
3. `motion_id` fails regex → `code=400, msg="invalid motion_id"`.
4. `motion_id` not in `running_->motions` → `code=400, msg="unknown motion_id '<id>'"`.
5. `client_->CallTriggerByName(desc.service_suffix, call_timeout_)`.
6. Map Trigger result: `success=true` → `code=0`; `success=false` → `code=502, msg="adapter rejected motion"`; RPC failure → `code=502, msg="adapter call failed"`.

Reuse the manager's existing `call_timeout_` (driven by `call_timeout_ms` in `server.yaml`, currently 2000). Do not introduce a new knob.

**`JsonResponseBuilder`** (`http/json_response_builder.{hpp,cpp}`):

```cpp
static std::string BuildMotionResponse(const InvokeMotionResult& result);
```

Wraps the existing `Envelope(code, msg, data_raw)` helper with a `data` object of `{motion_id, detail}` on success/502, or `null` on 400 outcomes that carry no useful payload.

**`http_server_runner_httplib.cpp`** — register one new route alongside `/start`:

```cpp
server->Post("/motion", [manager](const httplib::Request& request,
                                  httplib::Response& response) {
    if (!request.has_param("motion_id")) {
        SetJsonResponse(&response, 400,
                        JsonResponseBuilder::BuildBadRequestResponse(
                            "missing required parameter 'motion_id'"));
        return;
    }
    const std::string motion_id = TrimCopy(request.get_param_value("motion_id"));
    const auto result = manager->InvokeMotion(motion_id);
    const int http_status =
        (result.operation.code == 0)   ? 200 :
        (result.operation.code == 400) ? 400 : 502;
    SetJsonResponse(&response, http_status,
                    JsonResponseBuilder::BuildMotionResponse(result));
});
```

### 3. `adapter_go2`

In `Go2AdapterNode::OnSystemInfo`, populate the motion set on the `SystemInfoBuilder`:

| id | service_suffix | description |
|---|---|---|
| `stand_up` | `stand_up` | Recovery stand |
| `stop` | `stop` | Halt in place |
| `sit_down` | `sit_down` | Stop then sit down |
| `emergency_stop` | `emergency_stop` | Damp all joints |

`RegisterExtensions()` now exposes the same four public service suffixes: `stand_up`, `stop`, `sit_down`, `emergency_stop`.

### 4. `adapter_lynx`

In `LynxAdapterNode::OnSystemInfo`, declare the three posture motions exposed through the HTTP `/motion` API:

| id | service_suffix | SDK mapping |
|---|---|---|
| `stand_up` | `stand_up` | `SetMotionState(1)` |
| `soft_stop` | `soft_stop` | `SetMotionState(2)` |
| `sit_down` | `sit_down` | `SetMotionState(4)` |

Other Lynx extension services remain regular adapter-local services, but are not declared in the motion set and therefore are not invokable via `POST /motion`.

### 5. `adapter_fake`

Add two new Trigger extension services purely as the motion-dispatch test vehicle. Both register in `RegisterExtensions()` alongside the existing behavior-mode timers:

- `/adapter_fake/echo` — always returns `success=true, message="echo"`.
- `/adapter_fake/fail_motion` — always returns `success=false, message="forced failure"`.

In `OnSystemInfo`, declare both:

| id | service_suffix | description |
|---|---|---|
| `echo` | `echo` | Test success path |
| `fail_motion` | `fail_motion` | Test failure path |

## Data Flow

### Cached-at-start sequence

```
Client            SwitchServer                 Adapter
  │                   │                           │
  │ POST /start       │                           │
  │──────────────────▶│                           │
  │                   │  supervisor fork/exec ─▶  │ (spawn)
  │                   │  Connect (Trigger)        │
  │                   │──────────────────────────▶│
  │                   │◀─── success ─────────────│
  │                   │  state → Running          │
  │                   │                           │
  │                   │  SystemInfo (Trigger)     │
  │                   │──────────────────────────▶│
  │                   │◀── JSON with motions[] ──│
  │                   │  parse + validate         │
  │                   │  populate cache           │
  │◀──────────────────│  200 OK                   │
  │                                               │
```

### Motion invocation

```
Client            SwitchServer                 Adapter
  │ POST /motion?    │                           │
  │ motion_id=sit_down │                         │
  │──────────────────▶│                           │
  │                   │  validate regex           │
  │                   │  lookup "sit_down" in cache │
  │                   │    → "sit_down"           │
  │                   │  CallTriggerByName        │
  │                   │──────────────────────────▶│
  │                   │◀── success=true, msg ────│
  │◀──────────────────│  200 {code:0, ...}        │
```

## Error Handling

See the response table under **API Surface**. Concurrency: `InvokeMotion` takes the same mutex as `Start`/`Stop`, so a motion cannot race a lifecycle transition — it will either execute against a fully-running adapter or see a non-Running state and 400.

Malformed `/system_info` at start: warn-and-continue (see `AdapterRuntimeManager` above). An adapter that drops all motion declarations looks, to `/motion`, identical to an adapter that never declared any — both paths yield `"unknown motion_id '<id>'"`.

## Testing

- **`SystemInfoBuilder`**: no new dedicated test suite (none exists at workspace level). Validation paths exercised transparently via the integration walk below.
- **`adapter_fake`**: gains `echo` and `fail_motion` services as the motion-dispatch test vehicle. These are the minimum surface to walk every dispatch branch with no real robot.
- **Integration walk** (manual via `curl`, documented here for reproduction; launches `adapter_fake`):

  ```bash
  ros2 launch robot_switch_server robot_switch_system.launch.py
  curl -X POST 'http://localhost:9098/start?adapter_type=fake'

  curl 'http://localhost:9098/system_info'           # → motions=[echo, fail_motion]
  curl -X POST 'http://localhost:9098/motion?motion_id=echo'          # 200, code=0
  curl -X POST 'http://localhost:9098/motion?motion_id=fail_motion'   # 502, code=502
  curl -X POST 'http://localhost:9098/motion?motion_id=nope'          # 400, unknown
  curl -X POST 'http://localhost:9098/motion'                         # 400, missing param
  curl -X POST 'http://localhost:9098/stop'
  curl -X POST 'http://localhost:9098/motion?motion_id=echo'          # 400, no adapter
  ```

- **Real-hardware smoke** (post-merge, human-driven): verify go2 `stand_up`, `stop`, `sit_down`, `emergency_stop`, and lynx `stand_up`, `soft_stop`, `sit_down`.

## Out of scope / future work

- Parameterized motions (`{motion_id, params}` body) — would require extending `AdapterClient` beyond `Trigger`.
- Async / long-running motions with progress reporting.
- Per-motion permission gating or audit logging.
- Dynamic motion-set mutation without a start/stop cycle.
