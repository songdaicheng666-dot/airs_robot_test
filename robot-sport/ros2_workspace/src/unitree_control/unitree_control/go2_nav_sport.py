import os
import sys
import time

sys.path.append("/home/gs/workspace/projects/GS-HUB/ros2_ws/src/unitree_control/unitree_control/unitree_sdk2_python")

from gs_dev.go2.go2 import go2_client

import rclpy
from rclpy.node import Node
from geometry_msgs.msg import Twist

class CmdVelSubscriber(Node):
    def __init__(self):
        super().__init__('cmd_vel_subscriber')
        HUB_ID = os.environ.get("HUB_ID")
        self.subscription = self.create_subscription(
            Twist,
            f'/{HUB_ID}/cmd_vel',
            self.topic_callback,
            10)
        self.subscription  # prevent unused variable warning
        self.x_speed=0
        self.y_speed=0
        self.yaw_speed=0

    def topic_callback(self, msg):
        self.x_speed = msg.linear.x
        self.y_speed = msg.linear.y
        self.yaw_speed = msg.angular.z
        go2_client.go2_move_without_avoid(velocity_x=self.x_speed, velocity_y=self.y_speed, velocity_z=self.yaw_speed)
        self.get_logger().info(f"x_speed = {self.x_speed}, y_speed = {self.y_speed}, yaw_speed = {self.yaw_speed}")


def main(args=None):
    rclpy.init(args=args)
    cmd_vel_subscriber = CmdVelSubscriber()

    res = go2_client.movement_stand()
    # time.sleep(1)
    # res = go2_client.hello()
    try:
        rclpy.spin(cmd_vel_subscriber)
    except KeyboardInterrupt:
        pass
    finally:
        go2_client.smooth_stop()
        cmd_vel_subscriber.destroy_node()
        rclpy.shutdown()

if __name__ == "__main__":
    main()

