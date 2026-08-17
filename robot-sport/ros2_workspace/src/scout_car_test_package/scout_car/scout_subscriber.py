import os
import rclpy
from rclpy.node import Node
from geometry_msgs.msg import Twist  # 导入 Twist 消息类型
import time
from gs_usb.gs_usb import GsUsb
from gs_usb.gs_usb_frame import GsUsbFrame
from gs_usb.constants import CAN_EFF_FLAG
import threading
import math
class robot_move(Node):

    def __init__(self):
        """
        初始化
        """
        super().__init__('robot_move')
        self.HUB_ID = os.environ.get("HUB_ID")
        
        # 检查HUB_ID是否正确设置
        if self.HUB_ID is None:
            self.get_logger().error("HUB_ID 环境变量未设置！请设置HUB_ID环境变量")
            self.HUB_ID = "default"  # 设置默认值
        
        self.get_logger().info(f'[info] HUB_ID: {self.HUB_ID}')
        
        # 添加线程锁保护CAN设备访问
        self.can_lock = threading.Lock()
        
        # 初始化CAN设备可用标志
        self.can_device_available = False
        self.dev = None
        
        # 先创建ROS2订阅，确保回调函数能够正常工作
        self.start_read()
        
        # 然后尝试初始化CAN设备（如果失败不影响ROS2功能）
        self.init_can_device()

    def init_can_device(self):
        """
        初始化CAN设备，增加异常处理
        """
        try:
            self.devs = GsUsb.scan()

            if len(self.devs) == 0:
                self.get_logger().error("未找到gs_usb设备，CAN功能将不可用")
                return
                
            self.dev = self.devs[0]
            self.get_logger().info('[info]找到设备{0}'.format(str(self.dev)))

            if not self.dev.set_bitrate(500000):
                self.get_logger().error("无法设置gs_usb的比特率，CAN功能将不可用")
                return

            # 启动设备
            self.dev.start()
            self.dev.send(GsUsbFrame(can_id=0x421, data=[0x01]))
            self.get_logger().info('[info] 设备已初始化,设置模式为can模式')

            # 标记CAN设备可用
            self.can_device_available = True
            
            # 启动can数据读取线程
            can_data_thread = threading.Thread(target=self.get_can_data)
            can_data_thread.daemon = True  # 设为守护线程
            can_data_thread.start()
            
        except Exception as e:
            self.get_logger().error(f"CAN设备初始化失败: {str(e)}，将继续运行但CAN功能不可用")
            self.can_device_available = False

    def start_read(self):
        """
        订阅节点
        """
        topic_name='robot_move'
        subscription_topic = f'/{self.HUB_ID}/cmd_vel'
        self.get_logger().info(f'[info] 订阅话题: {subscription_topic}')
        
        # 订阅 /output_topic 话题，消息类型为 Twist
        self.subscription = self.create_subscription(
            Twist,
            subscription_topic,
            lambda msg: self.listener_callback(topic_name, msg),
            10)
        self.subscription  # 防止未使用的变量警告

    def listener_callback(self,topic_name, msg):
        # 处理接收到的 Twist 消息
        print('*'*10)
        print('*'*10)
        print('*'*10)
        self.get_logger().info('Received Twist message:')
        self.get_logger().info(f'Linear velocity (x): {msg.linear.x}')
        self.get_logger().info(f'Angular velocity (z): {msg.angular.z}')
        # 接入代码，实现控制
        self.contral_moniter(msg)
    
    def get_can_data(self):
        """
        循环读取can数据，后面做个list设置兴趣反馈帧,这个线程不可关闭，需要包活
        """ 
        while self.can_device_available and self.dev is not None:
            try:
                iframe = GsUsbFrame()  # 创建一个空的CAN帧对象用于接收数据
                with self.can_lock:  # 使用锁保护设备访问
                    if self.dev.read(iframe,1):  # 尝试从设备读取一帧数据，超时时间为1秒
                        self.get_logger().info("接收帧到返回帧:")  # 如果读取成功，打印接收到的帧
            except Exception as e:
                self.get_logger().error(f"CAN数据读取错误: {str(e)}")
                break

    def send_can_data(self,id,data):
        """
        发送帧函数
        """
        if not self.can_device_available or self.dev is None:
            self.get_logger().warning("CAN设备不可用，无法发送数据")
            return False
            
        try:
            control_frame=GsUsbFrame(can_id=id, data=data)
            with self.can_lock:  # 使用锁保护设备访问
                if self.dev.send(control_frame):
                    self.get_logger().info("发送控制帧: {}".format(control_frame))
                    return True
                else:
                    self.get_logger().warning("发送CAN帧失败")
                    return False
        except Exception as e:
            self.get_logger().error(f"发送CAN数据时出现异常: {str(e)}")
            return False

    def convert_velocity_to_command(self,linear_velocity, angular_velocity):
        """
        linear_velocity: 线速
        angular_velocity：角速度
        : 返回指令数据的十六进制表示 list
        """
        # 转换线速度：m/s 转 mm/s
        linear_velocity_mm_s = int(linear_velocity * 1000)
        
        # 检查线速度是否在有效范围内
        if not -1500 <= linear_velocity_mm_s <= 1500:
            raise ValueError("Linear velocity out of range [-1500, 1500] mm/s")
        
        # 转换角速度：rad/s 转 0.001 rad/s
        angular_velocity_scaled = int(angular_velocity * 400)
        self.get_logger().info(f'[info]转换后角速度: {angular_velocity_scaled}')
        # 检查角速度是否在有效范围内
        if not -523 <= angular_velocity_scaled <= 523:
            raise ValueError("Angular velocity out of range [-523, 523] * 0.001 rad/s")
        
        # 将线速度和角速度转换为有符号的16位整数
        linear_velocity_bytes = linear_velocity_mm_s.to_bytes(2, byteorder='big', signed=True)
        angular_velocity_bytes = angular_velocity_scaled.to_bytes(2, byteorder='big', signed=True)
        
        # 构建指令数据
        command_data = [
            linear_velocity_bytes[0],  # byte[0]: 线速度高八位
            linear_velocity_bytes[1],  # byte[1]: 线速度低八位
            angular_velocity_bytes[0], # byte[2]: 角速度高八位
            angular_velocity_bytes[1], # byte[3]: 角速度低八位
            0x00,                      # byte[4]: 保留
            0x00,
            0x00,
            0x00                       # byte[5]: 保留

        ]
        
        # 返回指令数据的十六进制表示
        return command_data

    def contral_moniter(self,msg):
        """
        控制代码
        """
        id=0x111
        liner=math.sqrt(msg.linear.x**2 + msg.linear.y**2 + msg.linear.z**2)
        angular=math.sqrt(msg.angular.x**2 + msg.angular.y**2 + msg.angular.z**2)
        
        try:
            data=self.convert_velocity_to_command(msg.linear.x,msg.angular.z)#转化速度到控制指令
            self.get_logger().info(f'[info]发送指令: {data}')
            self.send_can_data(id,data)
        except ValueError as e:
            self.get_logger().error(f"速度转换错误: {str(e)}")
        except Exception as e:
            self.get_logger().error(f"控制过程中出现异常: {str(e)}")

 
def main(args=None):

    rclpy.init(args=args)
    node = robot_move()




    rclpy.spin(node)
    rclpy.shutdown()



if __name__ == '__main__':
    main()
