import time
import sys
sys.path.append("/home/gs/workspace/projects/GS-HUB/ros2_ws/src/common_utils/common_utils/unitree_sdk2_python")
from dataclasses import dataclass

from gs_dev.go2.go2 import go2_client


if __name__ == "__main__":
    sc = go2_client.sport_client.Damp()
    time.sleep(1)
    res = go2_client.movement_stand()
    time.sleep(1)
    go2_client.smooth_stop()
    print(res)
