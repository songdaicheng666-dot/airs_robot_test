import os
import sys
import time
import rclpy
from rclpy.node import Node
from geometry_msgs.msg import Twist
from robot_sport.base_robot_control import BaseRobotControl
sys.path.append("/home/gs/workspace/projects/robot-sport/ros2_workspace/src/unitree_control/unitree_control/unitree_sdk2_python")
from gs_dev.go2.go2 import go2_client

class UnitreeGo2Control(BaseRobotControl):
    def __init__(self):
        super().__init__('unitree_go2_control')
        self.init_go2_sdk()  # 初始化机器狗SDK
    
    def init_go2_sdk(self):
        """初始化四足机器狗SDK"""
        try:
            self.go2_client = go2_client
            
            # 使机器狗进入站立状态
            result = self.go2_client.movement_stand()
            self.get_logger().info(f"Stand command result: {result}")
            
        except ImportError as e:
            self.get_logger().error(f"Go2 SDK import error: {str(e)}")
        except Exception as e:
            self.get_logger().error(f"Quadruped init failed: {str(e)}")
    
    def control_robot(self, msg):
        """四足机器狗运动控制实现"""
        if not hasattr(self, 'go2_client'):
            self.get_logger().warn("Go2 client not available")
            return
        
        try:
            # 全向运动控制
            self.go2_client.go2_move_without_avoid(
                velocity_x=msg.linear.x,
                velocity_y=msg.linear.y,
                velocity_z=msg.angular.z
            )
            self.get_logger().info(f"Quadruped moving: X={msg.linear.x}, Y={msg.linear.y}, Z={msg.angular.z}")
            
        except Exception as e:
            self.get_logger().error(f"Control error: {str(e)}")
    
    def destroy_node(self):
        """资源清理"""
        super().destroy_node()
        if hasattr(self, 'go2_client'):
            self.go2_client.smooth_stop()
            self.get_logger().info("Quadruped stopped safely")
