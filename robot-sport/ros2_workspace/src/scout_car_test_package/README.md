# SCOUT
一个简单驱动SCOUT小车的程序（如果PC不支持gs_usb，则只能演示一次，要再演示只能拔插USB
SCOUT车 演示流程：

1. 登录到orin上：

   ```bash
   ssh gs@192.168.31.29
   ```

2. 设置500k波特率和使能can-to-usb适配器：

   ```bash
   sudo ip link set can0 up type can bitrate 500000
   ```

3. 查看can口id：

   ```bash
   lsusb
      //查看结果 例如：
      gs@gs-desktop:~$ lsusb
      Bus 002 Device 002: ID 2c7c:0900 Quectel Wireless Solutions Co., Ltd. RM500U-CNV
      Bus 002 Device 001: ID 1d6b:0003 Linux Foundation 3.0 root hub
      Bus 001 Device 006: ID 1d50:606f OpenMoko, Inc. Geschwister Schneider CAN adapter
      Bus 001 Device 004: ID 0bda:b00c Realtek Semiconductor Corp. Bluetooth Radio
      Bus 001 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub
      //以上查询结果重点在：Bus 001 Device 006: ID 1d50:606f OpenMoko, Inc. Geschwister Schneider CAN adapter
   ```

4. 赋予can口权限：

   ```bash
   sudo chmod 666 /dev/bus/usb/001/006   //以3中的查询结果举例，其中001和006分别是查询到的Bus和Device
   ```

5. 进入scout_car_ws：

   ```bash
   cd scout_car_ws
   ```

6. source一下：

   ```
   source install/setup.bash
   ```

7. can口开启订阅：

   ```bash
   ros2 run scout_car scout_subscriber
   ```

7. 发布者发布速度、加速度等消息：

   ```bash
   ros2 run scout_car scout_publisher
   ```

小车就动了 =v=
