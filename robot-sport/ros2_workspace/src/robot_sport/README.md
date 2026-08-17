# ROBOT-SPORT

## 1. 简介
机器人本体运动控制仓库

## 2. 文件结构

```
.
├── robot_sport
│   ├── launch
│   │   └── py
│   │       └── robot_sport_launch.py  # launch文件
│   ├── package.xml
│   ├── README.md
│   ├── resource
│   │   └── robot_sport
│   ├── robot_sport
│   │   ├── base_robot_control.py      # 机器人本体运动基类
│   │   ├── __init__.py
│   │   ├── robot                      # 机器人本体运动子类
│   │   │   ├── __init__.py
│   │   │   ├── scout_car.py
│   │   │   └── unitree_go2.py
│   │   └── robot_sport.py             # 主节点
│   ├── setup.cfg
│   ├── setup.py
│   └── test
├── scout_car_test_package             # 松灵机器车运动依赖包
└── unitree_control                    # 宇树机器狗运动依赖包
```

## 3. 使用方法
1. 编译并导入环境
```
cd /home/gs/workspace/projects/robot-sport/ros2_workspace/
colcon build
source ./install/setup.bash
```
2. 运行launch文件
```
ros2 launch robot_sport robot_sport_launch.py robot_type:=<robot_type>
```
其中，<robot_type>为机器人本体类型，松灵机器车为ScoutCar，宇树机器狗为UnitreeGo2。

## 4. 添加机器人本体
1. 若有机器人本体运动依赖包，添加机器人本体运动依赖包至src目录下；
2. 在 src/robot_sport/robot_sport/robot 下写机器人本体运动子类，子类名字为<robot_type>Control，其中<robot_type>为机器人本体类型，子类需继承BaseRobotControl基类；
3. 子类写__init__、destroy_node方法，并重写control_robot方法。
4. def control_robot(self, msg)为/{self.HUB_ID}/cmd_vel话题接收的回调函数，msg为geometry_msgs.msg.Twist类型的数据，函数作用是控制机器人本体运动。
