
from unitree_sdk2py.core.channel import ChannelSubscriber, ChannelFactoryInitialize
from unitree_sdk2py.idl.default import unitree_go_msg_dds__SportModeState_
from unitree_sdk2py.idl.unitree_go.msg.dds_ import SportModeState_
from unitree_sdk2py.go2.sport.sport_client import (
    SportClient,
    PathPoint,
    SPORT_PATH_POINT_SIZE,
)
from unitree_sdk2py.go2.obstacles_avoid.obstacles_avoid_client import (
    ObstaclesAvoidClient
)

from gs_dev.utils.utils import get_interface_by_subnet, code_info_wrapper

# connector
subnet = '192.168.123.0/24'
network_interface: str = get_interface_by_subnet(subnet)
ChannelFactoryInitialize(0, network_interface)

class Go2Client:

    def __init__(self):

        self.sport_client = self.__client_init("sport_client")
        self.obstacles_avoid_client = self.__client_init("obstacles_avoid_client")

    def __client_init(self, client_type: str):
        client_map = {
            "sport_client": SportClient,
            "obstacles_avoid_client": ObstaclesAvoidClient,
        }
        client = client_map[client_type]()
        client.SetTimeout(5.0)
        client.Init()

        return client

    def go2_move_without_avoid(self, 
        velocity_x: float = 0.0, 
        velocity_y: float = 0.0, 
        velocity_z: float = 0.0
    ) -> int:
        if abs(velocity_x) < 0.001 and abs(velocity_y) < 0.001 and abs(velocity_z) < 0.001:
            return code_info_wrapper(0, "velocity 0")
        status_code = self.sport_client.Move(velocity_x, velocity_y, velocity_z)
        return code_info_wrapper(status_code, "movement_control_without_avoidance")

    def go2_move_with_avoid(self, 
        velocity_x: float = 0.0, 
        velocity_y: float = 0.0, 
        velocity_z: float = 0.0
    ) -> int:

        status_code = self.obstacles_avoid_client.Move(velocity_x, velocity_y, velocity_z)
        return code_info_wrapper(status_code, "movement_control_with_avoidance")

    def obstacles_avoid_switcher(self) -> int:
        try:
            status_code, avoid_status = self.obstacles_avoid_client.SwitchGet()
            status_code = code_info_wrapper(status_code, "get avoidance status", f"avoidance status: {avoid_status}")
            assert status_code == 0

            status_code = self.obstacles_avoid_client.SwitchSet(not avoid_status)
            status_code = code_info_wrapper(status_code, f"set avoidance status {not avoid_status}")
            assert status_code == 0

            return status_code

        except Exception as e:
            print("err:", str(e))
            return status_code

    def movement_stand(self) -> int:
        """
        进入可以运动的站立模式
        """
        status_code = self.sport_client.RecoveryStand()
        return code_info_wrapper(status_code, "movement_stand")

    def hello(self):
        status_code = self.sport_client.Hello()
        return code_info_wrapper(status_code, "hello")


    def emergency_stop(self):
        status_code = self.sport_client.Damp()
        return code_info_wrapper(status_code, "damp")
    
    def smooth_stop(self):
        status_code = self.sport_client.StopMove()
        status_code = self.sport_client.StandDown()
        return code_info_wrapper(status_code, "smooth_stop")

go2_client = Go2Client()