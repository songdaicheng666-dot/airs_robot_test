# Repository Guidelines

## Project Structure & Module Organization
`src/` contains the ROS 2 packages that make up the workspace: `robot_adapter_interfaces` for shared types and client helpers, `robot_switch_server` for the HTTP switch server plus launch/config files, `remote_controller` for the WebSocket/MQTT-to-`cmd_vel` bridge, and adapters such as `adapter_go2`, `adapter_lynx`, and `adapter_fake`. Package code lives in `include/` and `src/`; runtime config lives in `config/`; launch files are under `src/robot_switch_server/launch/`. `docs/` stores API notes and plans. Treat `build/`, `install/`, `log/`, and `.ros/` as generated output. `src/adapter_go2/unitree_sdk2/` is vendored SDK code, so keep local edits minimal and well justified.

## Build, Test, and Development Commands
Run `source /opt/ros/humble/setup.bash` before any build or test step. Use `colcon build` for a full workspace build, or `colcon build --packages-select robot_switch_server remote_controller adapter_go2` when iterating on specific packages. After building, run `source install/setup.bash`. Start the integrated system with `ros2 launch robot_switch_server robot_switch_system.launch.py`; this launches both `robot_switch_server_node` and `remote_controller_node`. For package tests, run `colcon test --packages-select remote_controller` and then `colcon test-result --verbose`. For live bridge checks, use `python3 src/remote_controller/test/test_responses.py` or `python3 src/remote_controller/test/test.py`.

## Coding Style & Naming Conventions
Use C++17. Follow the surrounding file style instead of reformatting broadly: core packages mostly use 4-space indentation, `snake_case` filenames, `PascalCase` classes/types, and `kName` constants. Keep ROS package and executable names aligned, for example `adapter_lynx` and `adapter_lynx_node`. Keep YAML and JSON config filenames lowercase, such as `server.yaml` and `remote_controller_config.json`.

## Testing Guidelines
`remote_controller` is the only package with checked-in GTest coverage today, using `test/test_*.cpp` plus `ament_lint_auto` under `BUILD_TESTING`. When changing other packages, at minimum do a targeted `colcon build` and a launch or service smoke test for the affected node. Include the exact commands you ran in your change summary or PR.

## Commit & Pull Request Guidelines
Git history follows Conventional Commits with package scopes, for example `feat(adapter_lynx): ...`, `refactor(robot_adapter_interfaces): ...`, and `chore(debian): ...`. Keep messages imperative and scope them to the package you changed. PRs should list affected packages, describe config or launch-file changes, summarize verification commands, and include sample `curl` requests or WebSocket payloads when API behavior changes.

## Configuration & Operational Notes
Keep defaults in `src/*/config/` usable for local bring-up. If a change affects `server.yaml`, MQTT topic construction, or `/workspace/.info/device_info.json`, call out the operational impact explicitly so deployers can update their environment safely.
