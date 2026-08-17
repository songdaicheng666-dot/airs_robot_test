# 运控服务接口

## 公共初始化流程（第一、二部分）:

### 初始化代码

说明：以下第一部分和第二部分接口调用均基于此初始化流程

```C
#include <iostream>
#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/go2/sport/sport_client.hpp>

using namespace unitree::robot;
using namespace unitree::robot::go2;

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "Usage: " << argv[0] << " networkInterface" << std::endl;
        return -1;
    }

    ChannelFactory::Instance()->Init(0, argv[1]);

    SportClient sport_client;
    sport_client.SetTimeout(10.0f);
    sport_client.Init();
    sport_client.WaitLeaseApplied();

    return 0;
}
```

### 初始化代码分析（未完成）

#### ChannelFactory::Instance()->Init(0, argv[1])

- 其具体实现封装在预编译库中
    
- 这一步是在初始化底层通信环境，并指定通信网卡。
    
- `0` 对应参数名 **`domainId`****，**`argv[1]` 对应参数名 **`networkInterface`**
    

#### 使用了什么协议和机制

- 这套初始化路径建立在 Unitree 封装的 DDS 通道之上，底层
    

依赖 `ddsc / ddscxx`，属于 Cyclone DDS 体系。

#### SportClient 初始化说明什么

- 其具体实现封装在预编译库中
    
- `WaitLeaseApplied()` 和 `LeaseClientPtr` 说明该体系存在 lease（控制权限/有效性）机制。表明控制客户端在正式执行控制接口前，需要等待 lease 生效
    

## 公共初始化流程（第三部分）:

### 初始化代码

说明：以下第三部分接口调用均基于此初始化流程，并默认放在HighStateHandler方法末尾

```C
#include <iostream>
#include <unistd.h>
#include <unitree/idl/go2/SportModeState_.hpp>
#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

using namespace unitree::robot;

#define TOPIC_HIGHSTATE "rt/sportmodestate"

void HighStateHandler(const void* message)
{
    unitree_go::msg::dds_::SportModeState_ state =
        *(unitree_go::msg::dds_::SportModeState_*)message;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "Usage: " << argv[0] << " networkInterface" << std::endl;
        return -1;
    }

    ChannelFactory::Instance()->Init(0, argv[1]);

    ChannelSubscriber<unitree_go::msg::dds_::SportModeState_> suber(TOPIC_HIGHSTATE);
    suber.InitChannel(HighStateHandler);

    while (true)
    {
        usleep(200000);
    }

    return 0;
}
```

### 初始化代码分析

  

#### ChannelFactory::Instance()->Init(0, argv[1])

- 这一步是在初始化底层通信环境，并指定通信网卡。
    
- `0` 对应参数名 **`domainId`**
    
- `argv[1]` 对应参数名 **`networkInterface`**
    

#### 使用了什么协议和机制

- 这套初始化路径建立在 Unitree 封装的 DDS 通道之上，底层
    

依赖 `ddsc / ddscxx`，属于 Cyclone DDS 体系。

#### SportClient 初始化说明什么

- 其具体实现封装在预编译库中
    
- `WaitLeaseApplied()` 和 `LeaseClientPtr` 说明该体系存在 lease（控制权限/有效性）机制。表明控制客户端在正式执行控制接口前，需要等待 lease 生效
    

## 第一部分：基础姿态/状态切换

### **Damp**

#### 基本信息

|   |   |
|---|---|
|**函数名**|Damp|
|**函数原型**|int32_t Damp()|
|**功能概述**|进入阻尼状态。|
|**参数**|无|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.Damp();
std::cout << "Damp() return = " << ret << std::endl;
```

### **BalanceStand**

#### 基本信息

|   |   |
|---|---|
|**函数名**|BalanceStand|
|**函数原型**|int32_t BalanceStand()|
|**功能概述**|平衡站立。|
|**参数**|无|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.BalanceStand();
std::cout << "BalanceStand() return = " << ret << std::endl;
```

### **StopMove**

#### 基本信息

|   |   |
|---|---|
|**函数名**|StopMove|
|**函数原型**|int32_t StopMove()|
|**功能概述**|停止移动。|
|**参数**|无|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.StopMove();
std::cout << "StopMove() return = " << ret << std::endl;
```

  

### **StandUp**

#### 基本信息

|   |   |
|---|---|
|**函数名**|StandUp|
|**函数原型**|int32_t StandUp()|
|**功能概述**|起立。|
|**参数**|无|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.StandUp();
std::cout << "StandUp() return = " << ret << std::endl;
```

  

### **StandDown**

#### 基本信息

|   |   |
|---|---|
|**函数名**|StandDown|
|**函数原型**|int32_t StandDown()|
|**功能概述**|趴下/降低站姿。|
|**参数**|无|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.StandDown();
std::cout << "StandDown() return = " << ret << std::endl;
```

  

### **RecoveryStand**

#### 基本信息

|   |   |
|---|---|
|**函数名**|RecoveryStand|
|**函数原型**|int32_t RecoveryStand()|
|**功能概述**|恢复站立。|
|**参数**|无|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.RecoveryStand();
std::cout << "RecoveryStand() return = " << ret << std::endl;
```

  

### **Sit**

#### 基本信息

|   |   |
|---|---|
|**函数名**|Sit|
|**函数原型**|int32_t Sit()|
|**功能概述**|坐下。|
|**参数**|无|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.Sit();
std::cout << "Sit() return = " << ret << std::endl;
```

  

### **RiseSit**

#### 基本信息

|   |   |
|---|---|
|**函数名**|RiseSit|
|**函数原型**|int32_t RiseSit()|
|**功能概述**|从坐姿恢复。|
|**参数**|无|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.RiseSit();
std::cout << "RiseSit() return = " << ret << std::endl;
```

## 第二部分：运动控制类接口

  

### **Move**

#### 基本信息

|   |   |
|---|---|
|**函数名**|Move|
|**函数原型**|int32_t Move(float vx, float vy, float vyaw)|
|**功能概述**|移动。|
|**参数**|`vx`：前后速度；`vy`：左右速度；`vyaw`：偏航角速度|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**|控制移动速度。设定的速度为机体坐标系表示下的速度。Move接口有两个特点：（1）运控部分不会对Move指令进行滤波；（2）最新的Move指令会维持1s。建议：使用该接口时，自行加滤波然后发送；在不使用Move时，发送Move(0,0,0)或者StopMove()|

#### 示例代码

```C
int32_t ret = sport_client.Move(vx, vy, vyaw);
std::cout << "Move(vx, vy, vyaw) return = " << ret << std::endl;

//实测代码示例 实测时应注意最新的Move指令会维持1s
int32_t ret = sport_client.Move(0.3f, 0.0f, 0.0f);
std::cout << "Move(0.3, 0, 0) return = " << ret << std::endl;
```

  

### **Euler**

#### 基本信息

|   |   |
|---|---|
|**函数名**|Euler|
|**函数原型**|int32_t Euler(float roll, float pitch, float yaw)|
|**功能概述**|设置机体姿态角|
|**参数**|`roll`：横滚角；`pitch`：俯仰角；`yaw`：偏航角|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**||

#### 示例代码

```C
int32_t ret = sport_client.Euler(roll, pitch, yaw);
std::cout << "Euler(roll, pitch, yaw) return = " << ret << std::endl;

//实测代码示例
int32_t ret = sport_client.Euler(0.4f, 0.55f, 0.0f);
std::cout << "Euler(0.4, 0.55, 0) return = " << ret << std::endl;
```

### **SpeedLevel**

#### 基本信息

|   |   |
|---|---|
|**函数名**|SpeedLevel|
|**函数原型**|int32_t SpeedLevel(int level)|
|**功能概述**|设置速度档位。|
|**参数**|`level`：速度档位枚举值；`-1` 为慢速，`0` 为正常，`1` 为快速|
|**返回值说明**|调用成功返回 `0`，否则返回相关错误码。|
|**备注**|实测返回值：`SpeedLevel(-1) = 0`，`SpeedLevel(0) = -1`（不知道为什么返回值是-1），`SpeedLevel(1) = 0`。|

#### 示例代码

```C
int32_t ret = sport_client.SpeedLevel(level);
std::cout << "SpeedLevel(level) return = " << ret << std::endl;

//实测代码示例
int32_t ret = sport_client.SpeedLevel(-1);
std::cout << "SpeedLevel(-1) return = " << ret << std::endl;
```

  

## 第三部分:信息获取部分

### **Position**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|std::array<float, 3> position()|
|**功能概述**|获取机器人三维位置。|
|**返回值说明**|三个值分别返回位置信息|
|**备注**||

#### 示例代码

```C
std::cout << "position: "
          << state.position()[0] << ", "
          << state.position()[1] << ", "
          << state.position()[2] << std::endl;
```

### **Velocity**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|std::array<float, 3> velocity()|
|**功能概述**|获取机器人三维速度。|
|**返回值说明**|三个值分别返回三个维度的速度|
|**备注**||

#### 示例代码

```C
std::cout << "velocity: "
          << state.velocity()[0] << ", "
          << state.velocity()[1] << ", "
          << state.velocity()[2] << std::endl;
```

### **BodyHeight**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|float body_height()|
|**功能概述**|获取机器人机体高度。|
|**返回值说明**|返回机器人高度|
|**备注**||

#### 示例代码

```C
std::cout << "body_height: "
          << state.body_height() << std::endl;
```

### **ErrorCode**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|uint32_t error_code()|
|**功能概述**|获取当前状态码 / 当前模式反馈信息。|
|**返回值说明**|返回状态码/信息|
|**备注**||

#### 示例代码

```C
std::cout << "error_code: "
          << state.error_code() << std::endl;
```

### **YawSpeed**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|float yaw_speed()|
|**功能概述**|获取偏航速度。|
|**返回值说明**|返回速度值。|
|**备注**||

#### 示例代码

```C
std::cout << "yaw_speed: "
          << state.yaw_speed() << std::endl;
```

### **Quaternion**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|std::array<float, 4> quaternion()|
|**功能概述**|获取 IMU 四元数姿态信息。|
|**返回值说明**|分别获取四个姿态的信息|
|**备注**||

#### 示例代码

```C
std::cout << "quaternion: "
          << state.imu_state().quaternion()[0] << ", "
          << state.imu_state().quaternion()[1] << ", "
          << state.imu_state().quaternion()[2] << ", "
          << state.imu_state().quaternion()[3] << std::endl;
```

### **Gyroscope**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|std::array<float, 3> gyroscope()|
|**功能概述**|获取 IMU 角速度信息。|
|**返回值说明**|分别获取三个角速度的值|
|**备注**||

#### 示例代码

```C
std::cout << "gyroscope: "
          << state.imu_state().gyroscope()[0] << ", "
          << state.imu_state().gyroscope()[1] << ", "
          << state.imu_state().gyroscope()[2] << std::endl;
```

### **Accelerometer**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|std::array<float, 3> accelerometer()|
|**功能概述**|获取 IMU 加速度信息。|
|**返回值说明**|分别获取三个加速度的值|
|**备注**||

#### 示例代码

```C
std::cout << "accelerometer: "
          << state.imu_state().accelerometer()[0] << ", "
          << state.imu_state().accelerometer()[1] << ", "
          << state.imu_state().accelerometer()[2] << std::endl;
```

### **RPY**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|std::array<float, 3> rpy()|
|**功能概述**|获取 IMU 欧拉角姿态信息。|
|**返回值说明**|分别获取三个姿态信息。|
|**备注**||

#### 示例代码

```C
std::cout << "rpy: "
          << state.imu_state().rpy()[0] << ", "
          << state.imu_state().rpy()[1] << ", "
          << state.imu_state().rpy()[2] << std::endl;
```

  

### **Temperature**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|int8_t temperature()|
|**功能概述**|获取 IMU 温度信息。|
|**返回值说明**|返回温度信息|
|**备注**||

#### 示例代码

```C
std::cout << "temperature: "
          << (int)state.imu_state().temperature() << std::endl;
```

## 第四部分 暂定电池及其他信息

### 初始化代码

说明：以下电池信息调用均基于此初始化流程，方法放至LowStateHandler方法末尾即可。

```C
#include <iostream>
#include <unistd.h>
#include <unitree/idl/go2/LowState_.hpp>
#include <unitree/robot/channel/channel_factory.hpp>
#include <unitree/robot/channel/channel_subscriber.hpp>

using namespace unitree::robot;

#define TOPIC_LOWSTATE "rt/lowstate"

void LowStateHandler(const void* message)
{
    unitree_go::msg::dds_::LowState_ state =
        *(unitree_go::msg::dds_::LowState_*)message;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cout << "Usage: " << argv[0] << " networkInterface" << std::endl;
        return -1;
    }

    ChannelFactory::Instance()->Init(0, argv[1]);

    ChannelSubscriber<unitree_go::msg::dds_::LowState_> suber(TOPIC_LOWSTATE);
    suber.InitChannel(LowStateHandler);

    while (true)
    {
        usleep(200000);
    }

    return 0;
}
```

**初始化代码分析：**

与之前的公共代码在通信框架上是一致的，均基于 `ChannelFactory` 和 `ChannelSubscriber` 建立订阅关系；区别在于第四部分订阅的是低层状态 topic `rt/lowstate`，对应消息类型 `LowState_`，主要用于获取电池/BMS/温度等底层状态信息。

### **BatterySOC**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|uint8_t soc()|
|**功能概述**|获取电池电量百分比|
|**返回值说明**|返回电量百分比|
|**备注**|实测结果：soc=47%|

#### 示例代码

```C
std::cout << "soc="
          << static_cast<int>(state.bms_state().soc())
          << "%" << std::endl;
```

  

### **BatteryTemperature**

#### 基本信息

|   |   |
|---|---|
|**方法原型**|`std::array<int8_t, 2> bq_ntc()` / `std::array<int8_t, 2> mcu_ntc()` / `int8_t temperature_ntc1()` / `int8_t temperature_ntc2()`|
|**功能概述**|获取电池/BMS 相关温度信息|
|**返回值说明**|返回温度信息|
|**备注**||

#### 示例代码

BMS 温度

```C
std::cout << "bq_ntc[0,1]=["
          << static_cast<int>(state.bms_state().bq_ntc()[0]) << ","
          << static_cast<int>(state.bms_state().bq_ntc()[1]) << "]"
          << std::endl;

std::cout << "mcu_ntc[0,1]=["
          << static_cast<int>(state.bms_state().mcu_ntc()[0]) << ","
          << static_cast<int>(state.bms_state().mcu_ntc()[1]) << "]"
          << std::endl;
```

低层温度

```C
std::cout << "temperature_ntc1="
          << static_cast<int>(state.temperature_ntc1()) << std::endl;

std::cout << "temperature_ntc2="
          << static_cast<int>(state.temperature_ntc2()) << std::endl;
```

实测结果

```C
bq_ntc[0,1]=[22,22]
mcu_ntc[0,1]=[25,24]
temperature_ntc1=34
temperature_ntc2=31
```
