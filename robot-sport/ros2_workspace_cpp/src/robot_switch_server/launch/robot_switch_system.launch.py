from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration, PathJoinSubstitution
from launch_ros.actions import Node
from launch_ros.substitutions import FindPackageShare


def generate_launch_description() -> LaunchDescription:
    default_config = PathJoinSubstitution(
        [FindPackageShare("robot_switch_server"), "config", "server.yaml"]
    )
    config_file = DeclareLaunchArgument(
        "config_file",
        default_value=default_config,
    )

    switch_server = Node(
        package="robot_switch_server",
        executable="robot_switch_server_node",
        output="screen",
        parameters=[LaunchConfiguration("config_file")],
    )

    remote_controller_config = PathJoinSubstitution(
        [FindPackageShare("remote_controller"), "config", "remote_controller_config.json"]
    )

    remote_ctrl = Node(
        package="remote_controller",
        executable="remote_controller_node",
        output="screen",
        additional_env={"REMOTE_CONTROLLER_CONFIG": remote_controller_config},
    )

    return LaunchDescription(
        [
            config_file,
            switch_server,
            remote_ctrl,
        ]
    )
