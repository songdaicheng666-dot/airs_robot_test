from launch_ros.actions import Node
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration

def generate_launch_description():
    declare_arg = DeclareLaunchArgument("robot_type", default_value="UnitreeGo2")

    robot_sport = Node(package="robot_sport", executable="robot_sport", name="robot_sport", output="screen", parameters=[{"robot_type": LaunchConfiguration("robot_type")}])
    return LaunchDescription([declare_arg, robot_sport])
