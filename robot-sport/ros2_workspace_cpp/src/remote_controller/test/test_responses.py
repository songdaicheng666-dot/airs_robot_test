#!/usr/bin/env python3
"""
WebSocket响应格式测试工具.

测试各种输入情况下的响应格式是否符合预期.
支持键盘交互式控制模式。
"""

import json
import sys
import time

from websocket import WebSocket

# 尝试导入keyboard库，如果没有则使用基本输入
try:
    import keyboard
    HAS_KEYBOARD = True
except ImportError:
    HAS_KEYBOARD = False

try:
    import msvcrt  # Windows
    HAS_MSVCRT = True
except ImportError:
    HAS_MSVCRT = False

try:
    import termios  # Unix/Linux
    import tty
    HAS_TERMIOS = True
except ImportError:
    HAS_TERMIOS = False


class ResponseTester:
    """WebSocket响应测试工具类."""

    def __init__(self, url="ws://localhost:9099"):
        """初始化测试工具."""
        self.url = url
        self.test_results = []
        self.ws = None
        self.running = False

        # 控制参数
        self.max_linear_speed = 1.0
        self.max_angular_speed = 1.0
        self.speed_step = 0.5

    def connect(self):
        """建立WebSocket连接."""
        self.ws = WebSocket()
        try:
            self.ws.connect(self.url)
            print(f"✓ 已连接到 {self.url}")
            return True
        except Exception as e:
            print(f"✗ 连接失败: {e}")
            return False

    def send_and_receive(self, data, test_name=None):
        """发送数据并接收响应."""
        try:
            # 发送数据
            json_msg = json.dumps(data) if isinstance(data, dict) else data
            self.ws.send(json_msg)

            # 接收响应
            response = self.ws.recv()
            response_data = json.loads(response)

            if test_name:
                # 记录测试结果
                result = {
                    "test": test_name,
                    "input": data,
                    "response": response_data,
                    "success": True
                }
                self.test_results.append(result)

            return response_data

        except Exception as e:
            if test_name:
                result = {
                    "test": test_name,
                    "input": data,
                    "error": str(e),
                    "success": False
                }
                self.test_results.append(result)
            else:
                print(f"发送命令失败: {e}")
            return None

    def send_velocity_command(self, linear_x, angular_z):
        """发送速度控制命令."""
        data = {
            "linear_x": linear_x,
            "angular_z": angular_z
        }
        response = self.send_and_receive(data)
        if response and response.get("code") == 0:
            print(f"✓ 速度设置: linear_x={linear_x:.1f}, angular_z={angular_z:.1f}")
        elif response:
            print(
                f"✗ 速度设置失败: 错误码={response.get('code')}, "
                f"信息={response.get('msg', 'Unknown error')}"
            )

    def get_key_cross_platform(self):
        """跨平台获取按键输入."""
        if HAS_KEYBOARD:
            # 使用keyboard库（推荐）
            event = keyboard.read_event()
            if event.event_type == keyboard.KEY_DOWN:
                return event.name.lower()
            return None
        elif HAS_MSVCRT:
            # Windows系统
            if msvcrt.kbhit():
                key = msvcrt.getch().decode('utf-8').lower()
                return key
            return None
        elif HAS_TERMIOS:
            # Unix/Linux系统
            try:
                fd = sys.stdin.fileno()
                old_settings = termios.tcgetattr(fd)
                tty.setraw(sys.stdin.fileno())
                key = sys.stdin.read(1).lower()
                termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
                return key
            except Exception:
                return None
        else:
            # 回退到基本输入（阻塞式）
            try:
                key = input().lower()
                return key[0] if key else None
            except Exception:
                return None

    def interactive_control(self):
        """交互式键盘控制模式."""
        print("\n=== 交互式键盘控制模式 ===")
        print("控制说明:")
        print("  w - 前进")
        print("  s - 后退")
        print("  a - 左转")
        print("  d - 右转")
        print("  空格 - 停止")
        print("  q - 退出控制模式")
        print("  + - 增加速度")
        print("  - - 减少速度")
        print(f"当前最大速度: 线性={self.max_linear_speed}, 角度={self.max_angular_speed}")
        print("-" * 50)

        if not self.connect():
            return

        self.running = True
        current_linear = 0.0
        current_angular = 0.0

        try:
            while self.running:
                if not HAS_KEYBOARD and not HAS_MSVCRT and not HAS_TERMIOS:
                    # 回退模式：使用简单输入
                    print("请输入命令 (w/a/s/d/space/q/+/-): ", end='')
                    key = input().strip().lower()
                    if key == '':
                        key = 'space'
                else:
                    # 实时按键检测
                    key = self.get_key_cross_platform()
                    if not key:
                        time.sleep(0.05)  # 短暂延迟避免CPU过度使用
                        continue

                # 处理按键
                if key == 'q':
                    print("\n退出控制模式")
                    break
                elif key == 'w':
                    current_linear = self.max_linear_speed
                    current_angular = 0.0
                    print("前进")
                elif key == 's':
                    current_linear = -self.max_linear_speed
                    current_angular = 0.0
                    print("后退")
                elif key == 'a':
                    current_linear = 0.0
                    current_angular = self.max_angular_speed
                    print("左转")
                elif key == 'd':
                    current_linear = 0.0
                    current_angular = -self.max_angular_speed
                    print("右转")
                elif key == 'space' or key == ' ':
                    current_linear = 0.0
                    current_angular = 0.0
                    print("停止")
                elif key == '+' or key == '=':
                    self.max_linear_speed = min(5.0, self.max_linear_speed + self.speed_step)
                    self.max_angular_speed = min(5.0, self.max_angular_speed + self.speed_step)
                    print(f"速度增加: 线性={self.max_linear_speed:.1f}, 角度={self.max_angular_speed:.1f}")
                    continue
                elif key == '-':
                    self.max_linear_speed = max(0.5, self.max_linear_speed - self.speed_step)
                    self.max_angular_speed = max(0.5, self.max_angular_speed - self.speed_step)
                    print(f"速度减少: 线性={self.max_linear_speed:.1f}, 角度={self.max_angular_speed:.1f}")
                    continue
                else:
                    continue

                # 发送速度命令
                self.send_velocity_command(current_linear, current_angular)

        except KeyboardInterrupt:
            print("\n收到中断信号，正在停止...")
        finally:
            # 发送停止命令
            self.send_velocity_command(0.0, 0.0)
            if self.ws:
                self.ws.close()
            self.running = False

    def test_valid_commands(self):
        """测试有效命令."""
        print("\n=== 测试有效命令 ===")

        test_cases = [
            ({"linear_x": 1.0, "angular_z": 0.5}, "正常速度命令"),
            ({"linear_x": 0.0, "angular_z": 0.0}, "停止命令"),
            ({"linear_x": -2.0, "angular_z": -1.0}, "负值速度命令"),
            ({"linear_x": 4.9, "angular_z": 3.1}, "接近极限值"),
        ]

        for data, test_name in test_cases:
            print(f"\n测试: {test_name}")
            response = self.send_and_receive(data, test_name)
            if response:
                self.print_response_summary(response)

    def test_validation_errors(self):
        """测试验证错误."""
        print("\n=== 测试验证错误 ===")

        test_cases = [
            ({}, "缺少所有字段"),
            ({"linear_x": 1.0}, "缺少angular_z"),
            ({"angular_z": 0.5}, "缺少linear_x"),
            ({"linear_x": "not_a_number", "angular_z": 0.5}, "linear_x不是数字"),
            ({"linear_x": 1.0, "angular_z": "not_a_number"}, "angular_z不是数字"),
            ({"linear_x": 10.0, "angular_z": 0.5}, "linear_x超出范围"),
            ({"linear_x": 1.0, "angular_z": 5.0}, "angular_z超出范围"),
            ({"linear_x": float('inf'), "angular_z": 0.5}, "linear_x为无穷大"),
            ({"linear_x": float('nan'), "angular_z": 0.5}, "linear_x为NaN"),
        ]

        for data, test_name in test_cases:
            print(f"\n测试: {test_name}")
            response = self.send_and_receive(data, test_name)
            if response:
                self.print_error_response(response)

    def test_json_errors(self):
        """测试JSON格式错误."""
        print("\n=== 测试JSON格式错误 ===")

        test_cases = [
            ('{"linear_x": 1.0, "angular_z":}', "不完整的JSON"),
            ('not_json_at_all', "完全不是JSON"),
            ('', "空字符串"),
            ('null', "null值"),
            ('"just_a_string"', "字符串而非对象"),
            ('[1, 2, 3]', "数组而非对象"),
        ]

        for data, test_name in test_cases:
            print(f"\n测试: {test_name}")
            response = self.send_and_receive(data, test_name)
            if response:
                self.print_error_response(response)

    def print_response_summary(self, response):
        """打印响应摘要."""
        if response.get("code") == 0:
            print("  ✓ 成功")
            print(f"  消息: {response.get('msg', 'N/A')}")
            print(f"  请求ID: {response.get('requestId', 'N/A')}")

            data = response.get("data", {})
            if data:
                velocity = data.get("velocity", {})
                target = data.get("target", {})
                linear_x = velocity.get('linear_x')
                angular_z = velocity.get('angular_z')
                print(f"  速度: linear_x={linear_x}, angular_z={angular_z}")
                print(f"  目标: {target.get('topic')} "
                      f"(Hub: {target.get('hub_id')})")
        else:
            print("  ✗ 失败")
            self.print_error_response(response)

    def print_error_response(self, response):
        """打印错误响应详情."""
        print(f"  错误码: {response.get('code', 'N/A')}")
        print(f"  错误信息: {response.get('msg', 'N/A')}")
        print(f"  请求ID: {response.get('requestId', 'N/A')}")

        # 如果有额外的错误详情
        if 'error' in response:
            error = response.get("error", {})
            if error.get("field"):
                print(f"  问题字段: {error.get('field')}")
            if error.get("suggestion"):
                print(f"  建议: {error.get('suggestion')}")

    def run_all_tests(self):
        """运行所有测试."""
        print("WebSocket响应格式测试开始")
        print("=" * 50)

        if not self.connect():
            return

        try:
            self.test_valid_commands()
            self.test_validation_errors()
            self.test_json_errors()

        finally:
            self.ws.close()
            print("\n" + "=" * 50)
            print(f"测试完成! 总计 {len(self.test_results)} 个测试")

            # 统计结果
            successful = sum(1 for r in self.test_results if r["success"])
            failed_count = len(self.test_results) - successful
            print(f"成功: {successful}, 失败: {failed_count}")

    def print_detailed_results(self):
        """打印详细测试结果."""
        print("\n=== 详细测试结果 ===")
        for i, result in enumerate(self.test_results, 1):
            print(f"\n{i}. {result['test']}")
            if result["success"]:
                response = result["response"]
                print(f"   输入: {result['input']}")
                print(f"   成功: {response.get('code') == 0}")
                print(f"   消息: {response.get('msg')}")
                print(f"   错误码: {response.get('code')}")
            else:
                print(f"   输入: {result['input']}")
                print(f"   错误: {result['error']}")


if __name__ == "__main__":
    print("WebSocket 运动控制工具")
    print("=" * 30)
    print("请选择模式:")
    print("1. 运行自动化测试")
    print("2. 交互式键盘控制")
    print("3. 退出")

    while True:
        try:
            choice = input("\n请输入选择 (1/2/3): ").strip()

            if choice == '1':
                print("\n启动自动化测试模式...")
                tester = ResponseTester()
                tester.run_all_tests()
                # 可选：打印详细结果
                # tester.print_detailed_results()
                break

            elif choice == '2':
                print("\n启动交互式控制模式...")
                controller = ResponseTester()
                controller.interactive_control()
                break

            elif choice == '3':
                print("再见！")
                break

            else:
                print("无效选择，请输入 1、2 或 3")

        except KeyboardInterrupt:
            print("\n\n程序被中断，再见！")
            break
        except Exception as e:
            print(f"\n错误: {e}")
            break
