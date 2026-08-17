import threading
from gs_usb.gs_usb import GsUsb
from gs_usb.gs_usb_frame import GsUsbFrame
from robot_sport.base_robot_control import BaseRobotControl

class ScoutCarControl(BaseRobotControl):
    def __init__(self):
        super().__init__('scout_car_control')
        self.init_can_device()  # 初始化CAN设备
        
    def init_can_device(self):
        """初始化CAN通信设备"""
        try:
            # CAN设备初始化流程
            self.can_lock = threading.Lock()
            self.can_device_available = False
            self.terminate_can_thread = False
            
            self.devs = GsUsb.scan()
            if not self.devs:
                self.get_logger().error("No gs_usb devices found")
                return
            
            self.dev = self.devs[0]
            self.get_logger().info(f"Found CAN device: {self.dev}")
            
            if not self.dev.set_bitrate(500000):
                self.get_logger().error("Failed to set bitrate")
                return
                
            self.dev.start()
            self.dev.send(GsUsbFrame(can_id=0x421, data=[0x01]))
            self.get_logger().info('[info] 设备已初始化,设置模式为can模式')
            self.can_device_available = True
            
            # 启动CAN数据读取线程
            self.can_thread = threading.Thread(target=self.get_can_data)
            self.can_thread.daemon = True
            self.can_thread.start()
            
        except ImportError as e:
            self.get_logger().error(f"CAN module import error: {str(e)}")
            self.can_device_available = False
        except Exception as e:
            self.get_logger().error(f"CAN init failed: {str(e)}")
            self.can_device_available = False
    
    def control_robot(self, msg):
        """松灵车运动控制实现"""
        if not hasattr(self, 'dev') or not self.can_device_available:
            self.get_logger().warn("CAN device not available")
            return
        
        try:
            linear = msg.linear.x
            angular = msg.angular.z
            
            # 单位转换与范围校验
            linear_mm_s = int(linear * 1000)
            angular_scaled = int(angular * 400)
            
            if not -1500 <= linear_mm_s <= 1500:
                self.get_logger().warn(f"Linear speed out of range: {linear_mm_s}")
                return
                
            if not -523 <= angular_scaled <= 523:
                self.get_logger().warn(f"Angular speed out of range: {angular_scaled}")
                return
            
            # CAN帧构造
            linear_bytes = linear_mm_s.to_bytes(2, 'big', signed=True)
            angular_bytes = angular_scaled.to_bytes(2, 'big', signed=True)
            
            data = [
                linear_bytes[0], linear_bytes[1],
                angular_bytes[0], angular_bytes[1],
                0x00, 0x00, 0x00, 0x00
            ]
            
            # 发送控制指令
            frame = GsUsbFrame(can_id=0x111, data=data)
            with self.can_lock:
                if self.dev.send(frame):
                    self.get_logger().info(f"发送控制帧: {frame}")
                else:
                    self.get_logger().warn("发送CAN帧失败")
                    
        except Exception as e:
            self.get_logger().error(f"发送CAN数据时出现异常: {str(e)}")
    
    def get_can_data(self):
        """CAN数据读取线程"""
        while not self.terminate_can_thread and self.dev:
            try:
                iframe = GsUsbFrame()
                with self.can_lock:
                    if self.dev.read(iframe, 1):
                        self.get_logger().info(f"接收帧到返回帧")
            except Exception as e:
                self.get_logger().error(f"CAN数据读取错误: {str(e)}")
                break
    
    def destroy_node(self):
        """资源清理"""
        super().destroy_node()
        if hasattr(self, 'dev'):
            self.terminate_can_thread = True
            if self.can_thread.is_alive():
                self.can_thread.join(timeout=1.0)
            self.dev.stop()
            self.get_logger().info("CAN device stopped")
