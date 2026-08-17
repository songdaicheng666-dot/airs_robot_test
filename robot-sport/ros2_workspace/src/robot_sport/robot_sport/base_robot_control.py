import os
from rclpy.node import Node
from geometry_msgs.msg import Twist

class BaseRobotControl(Node):
    def __init__(self, node_name):
        super().__init__(node_name)
        self.HUB_ID = os.environ.get("HUB_ID", "default_HUB_ID")
        self.get_logger().info(f"[Config] HUB_ID: {self.HUB_ID}")
        
        # 创建统一的话题订阅
        subscription_topic = f'/{self.HUB_ID}/cmd_vel'
        self.subscription = self.create_subscription(
            Twist,
            subscription_topic,
            self.listener_callback,
            10)
        self.get_logger().info(f"Subscribed to {subscription_topic}")
    
    def listener_callback(self, msg):
        """统一的消息处理入口"""
        self.get_logger().info(f"Received: Linear.x={msg.linear.x}, Angular.z={msg.angular.z}")
        self.control_robot(msg)
    
    def control_robot(self, msg):
        """抽象方法，由子类实现具体控制逻辑"""
        raise NotImplementedError("Subclasses must implement control_robot method")
    
    def destroy_node(self):
        """公共资源清理"""
        super().destroy_node()
