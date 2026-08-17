import psutil
import socket
import ipaddress
from typing import List

def get_interface_by_subnet(subnet: str) -> str:
    """
    获取符合指定网段的网口名称列表。
    
    :param subnet: 目标网段，以 CIDR 表示，例如 '192.168.123.0/24'
    :return: 包含符合条件的网口名称的列表。如果没有匹配的网口，返回空列表。
    """
    # 获取系统中所有网络接口的地址信息
    interfaces = psutil.net_if_addrs()
    matching_interfaces = []  # 用于存储符合条件的网口名称
    target_network = ipaddress.ip_network(subnet, strict=False)  # 解析目标网段

    for iface_name, iface_addresses in interfaces.items():
        # 遍历每个网口的地址信息
        for address in iface_addresses:
            if address.family == socket.AF_INET:  # 只处理 IPv4 地址
                try:
                    ip_addr = ipaddress.ip_address(address.address)  # 解析 IP 地址
                    if ip_addr in target_network:  # 判断 IP 是否属于目标网段
                        matching_interfaces.append(iface_name)  # 添加符合条件的网口名称
                        break
                except ValueError:
                    # 跳过无法解析的 IP 地址
                    continue
    
    return matching_interfaces[0]

def code_info_wrapper(code: int, info: str = "", debug_info:str = "") -> int:
    print(debug_info)
    if code != 0:
        print(f"{info} Fail: {code}")
        return code
    return 0

if __name__ == "__main__":
    # 示例用法
    subnet = '192.168.123.0/24'
    interfaces = get_interface_by_subnet(subnet)

    if interfaces:
        print(f"符合网段 {subnet} 的网口: {interfaces}")
    else:
        print(f"未找到符合网段 {subnet} 的网口")