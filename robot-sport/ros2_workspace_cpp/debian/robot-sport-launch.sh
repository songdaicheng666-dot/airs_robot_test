#!/bin/bash
set -eo pipefail

export ROS_DISTRO="${ROS_DISTRO:-humble}"
export RCUTILS_COLORIZED_OUTPUT="${RCUTILS_COLORIZED_OUTPUT:-1}"
export AMENT_TRACE_SETUP_FILES="${AMENT_TRACE_SETUP_FILES:-}"

source "/opt/ros/${ROS_DISTRO}/setup.bash"

if [[ -n "${ROBOT_SPORT_CONFIG_FILE:-}" ]]; then
    exec ros2 launch robot_switch_server robot_switch_system.launch.py \
        "config_file:=${ROBOT_SPORT_CONFIG_FILE}"
fi

exec ros2 launch robot_switch_server robot_switch_system.launch.py
