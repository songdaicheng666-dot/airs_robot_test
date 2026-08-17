#!/usr/bin/env python3
import json
import time

from websocket import WebSocket


# 配置参数
WS_URL = "ws://localhost:9099"  # WebSocket 服务器地址
HZ = 20                          # 发送频率 (Hz)
DURATION = 10                    # 测试持续时间 (秒)
LINEAR_START = 0.5               # 起始线速度
ANGULAR_Z = 0.3                  # 固定角速度
INCREMENT_PER_SECOND = 1.0       # 线速度每秒增量

# 计算步长和总步数
INCREMENT_PER_STEP = INCREMENT_PER_SECOND / HZ
TOTAL_STEPS = HZ * DURATION

# 当前线速度
linear_x = LINEAR_START

print("开始 WebSocket 测试")
print(f"目标: {HZ}Hz 频率, 持续 {DURATION}秒")
print(f"初始线速度: {LINEAR_START}, 每秒增加: {INCREMENT_PER_SECOND}")
print(f"固定角速度: {ANGULAR_Z}")
print("=" * 50)

# 创建 WebSocket 连接
ws = WebSocket()
ws.connect(WS_URL)
print("已连接到 WebSocket 服务器")

for i in range(1, TOTAL_STEPS + 1):
    # 构建 JSON 消息
    payload = {"linear_x": linear_x, "angular_z": ANGULAR_Z}
    json_msg = json.dumps(payload)

    # 发送消息
    ws.send(json_msg)

    # 显示发送信息
    print(f"发送 [{i:2d}/{TOTAL_STEPS}]: "
          f"线速度={linear_x:.2f} 角速度={ANGULAR_Z:.1f}")

    # 尝试接收响应（非阻塞）
    try:
        response = ws.recv()
        response_data = json.loads(response)

        if response_data.get("success", False):
            metadata = response_data.get("metadata", {})
            processing_time = metadata.get("processing_time_ms", 0)
            print(f"✓ 成功 - 处理时间: {processing_time:.2f}ms")

            # 显示详细数据（仅在第一次）
            if i == 1:
                data = response_data.get("data", {})
                velocity = data.get("velocity", {})
                target = data.get("target", {})
                print(f"  -> 发布到话题: {target.get('topic', 'N/A')}")
                print(f"  -> Hub ID: {target.get('hub_id', 'N/A')}")
        else:
            error = response_data.get("error", {})
            print(f"✗ 错误 [{error.get('code', 'UNKNOWN')}]: "
                  f"{error.get('message', '未知错误')}")
            if error.get("field"):
                print(f"  -> 字段: {error.get('field')}")
            if error.get("suggestion"):
                print(f"  -> 建议: {error.get('suggestion')}")
    except Exception as e:
        print(f"⚠ 响应解析失败: {e}")

    # 更新线速度
    linear_x += INCREMENT_PER_STEP

    # 按频率休眠
    time.sleep(1 / HZ)

# 关闭连接
ws.close()
print("=" * 50)
print(f"测试完成! 最终线速度: {linear_x:.2f}")
