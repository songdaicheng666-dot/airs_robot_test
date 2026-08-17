import time
from unitree_sdk2py.core.channel import ChannelSubscriber, ChannelFactoryInitialize
from unitree_sdk2py.idl.default import unitree_go_msg_dds__LowState_
from unitree_sdk2py.idl.unitree_go.msg.dds_ import LowState_

from gs_dev.utils.utils import get_interface_by_subnet, code_info_wrapper

# connector
subnet = '192.168.123.0/24'
network_interface: str = get_interface_by_subnet(subnet)
ChannelFactoryInitialize(0, network_interface)


def LowStateHandler(msg: LowState_):
    
    print("Battery state: 电量: ", msg.bms_state.soc)
    print("Battery state: 充放电信息: ", msg.bms_state.current)
    print("Battery state: 充电循环次数: ", msg.bms_state.cycle)
    print("Battery state: bq_ntc: ", msg.bms_state.bq_ntc)
    print("Battery state: mcu_ntc: ", msg.bms_state.mcu_ntc)

sub = ChannelSubscriber("rt/lowstate", LowState_)
sub.Init(LowStateHandler, 10)
while True:
    time.sleep(10.0)


