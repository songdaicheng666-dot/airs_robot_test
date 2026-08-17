#!/bin/sh
### BEGIN INIT INFO
# Provides:          land.sh
# Required-start:    $local_fs $remote_fs $network $syslog
# Required-Stop:     $local_fs $remote_fs $network $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: starts the svnd.sh daemon
# Description:       starts svnd.sh using start-stop-daemon
### END INIT INFO

# CAN kernel modules.
modprobe can
modprobe can-raw
modprobe can-bcm
modprobe can-gw
modprobe can_dev
modprobe mttcan
modprobe gs_usb

sleep 0.5
busybox devmem 0x0c303000 32 0x0000C400
busybox devmem 0x0c303008 32 0x0000C458
busybox devmem 0x0c303010 32 0x0000C400
busybox devmem 0x0c303018 32 0x0000C458

# Temporary fixed numbering: can0/can1 are onboard and can2 is Scout USB-CAN.
ip link set can0 down 2>/dev/null || true
ip link set can0 type can bitrate 1000000 restart-ms 0 loopback off
ip link set can0 up

ip link set can1 down 2>/dev/null || true
ip link set can1 type can bitrate 1000000 restart-ms 0 loopback off
ip link set can1 up

can2_wait=0
while [ ! -e /sys/class/net/can2 ] && [ "${can2_wait}" -lt 20 ]; do
    sleep 0.5
    can2_wait=$((can2_wait + 1))
done
if [ -e /sys/class/net/can2 ]; then
    ip link set can2 down 2>/dev/null || true
    ip link set can2 type can bitrate 500000 restart-ms 100 loopback off
    ip link set can2 up
else
    logger -t can_add_server "Scout USB-CAN interface can2 was not found"
fi

# SPI-1
busybox devmem 0x0243d008 w 0x00000400
busybox devmem 0x0243d018 w 0x00000450
busybox devmem 0x0243d028 w 0x00000400
busybox devmem 0x0243d038 w 0x00000400
busybox devmem 0x0243d040 w 0x00000400

kernel=$(uname -r)
if [ "${kernel}" = "5.15.148-rt-ms" ]; then
    sleep 5
    modprobe cfg80211
    insmod /lib/modules/wlan_cnss_core_pcie.ko
    insmod /lib/modules/wlan.ko country_code=CN

    sleep 10
    iw dev wlan0 interface add ap0 type __ap

    sleep 20
    /usr/sbin/ifconfig ap0 0.0.0.0
    /usr/sbin/ip link set ap0 up
    /usr/sbin/ip addr add 192.168.80.1/24 dev ap0
    service udhcpd restart
fi

exit 0
