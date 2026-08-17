import rclpy
import pkgutil
import importlib
from robot_sport.base_robot_control import BaseRobotControl

def discover_robot_classes():
    """自动发现 robot 包中所有以 Control 结尾的控制类"""
    robot_classes = {}
    package_name = 'robot_sport.robot'
    
    # 遍历 robot 包中的所有模块
    package = importlib.import_module(package_name)
    for _, module_name, _ in pkgutil.walk_packages(
        package.__path__,
        prefix=package.__name__ + '.'
    ):
        try:
            # 导入模块
            module = importlib.import_module(module_name)
            
            # 查找以 "Control" 结尾的类
            for attr_name in dir(module):
                attr = getattr(module, attr_name)
                
                # 检查：是类、以Control结尾、且是BaseRobotControl子类
                if (isinstance(attr, type) and 
                    attr_name.endswith("Control") and 
                    issubclass(attr, BaseRobotControl) and
                    attr is not BaseRobotControl):
                    
                    # 提取机器人类型名（去掉Control后缀并小写）
                    robot_type = attr_name[:-7]
                    robot_classes[robot_type] = attr
        except ImportError as e:
            rclpy.logging.get_logger('discovery').error(f"加载模块失败 {module_name}: {str(e)}")
    
    return robot_classes

def main(args=None):
    rclpy.init(args=args)
    logger = rclpy.logging.get_logger('main')
    
    # 获取可用机器人类型映射 {类型名: 控制类}
    robot_classes = discover_robot_classes()
    logger.info(f"发现机器人类型: {list(robot_classes.keys())}")
    
    # 获取环境变量选择类型
    param_loader = rclpy.create_node('param_loader')
    param_loader.declare_parameter('robot_type', '')
    target_type = param_loader.get_parameter('robot_type').get_parameter_value().string_value.strip()
    param_loader.destroy_node()
    
    try:
        if not target_type:
            raise ValueError("ROBOT_TYPE 环境变量未设置")
            
        # 查找匹配的控制类
        control_class = robot_classes.get(target_type)
        if not control_class:
            raise ValueError(f"不支持的 ROBOT_TYPE: {target_type}")
            
        node = control_class()
        logger.info(f"已启动机器人控制器: {target_type}")
        rclpy.spin(node)
        
    except (ValueError, KeyError) as e:
        logger.error(f"配置错误: {str(e)}")
    except KeyboardInterrupt:
        pass
    except Exception as e:
        logger.error(f"运行时错误: {str(e)}", exc_info=True)
    finally:
        if 'node' in locals():
            node.destroy_node()
        rclpy.shutdown()

if __name__ == '__main__':
    main()