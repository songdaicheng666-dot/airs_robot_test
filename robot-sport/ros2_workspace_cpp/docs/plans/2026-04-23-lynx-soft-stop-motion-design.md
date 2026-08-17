# Lynx Soft Stop Motion Design

**Date**: 2026-04-23
**Status**: Approved

## Goal

Extend the Lynx adapter's discrete motion set so the unified HTTP `POST /motion` API can trigger all three operator-approved posture/motion-state transitions:

- `stand_up` -> `SetMotionState(1)`
- `soft_stop` -> `SetMotionState(2)`
- `sit_down` -> `SetMotionState(4)`

## Design

Keep the current public naming scheme and implementation style:

- Add `soft_stop` to `LynxAdapterNode::OnSystemInfo()` via `SystemInfoBuilder::SetMotions(...)`
- Register a new `/lynx/soft_stop` Trigger service in `RegisterExtensions()`
- Implement `OnSoftStop(...)` using `sdk_.SetMotionState(2)` with the same success/failure reporting pattern used by `stand_up` and `sit_down`
- Update API/spec documentation so callers know Lynx now exposes three motion ids through `/motion`

## Why This Approach

This is the smallest change that keeps the unified motion contract intact. The switch server already dispatches motions dynamically from the adapter-declared `motions` array, so no server-side code needs to change.
