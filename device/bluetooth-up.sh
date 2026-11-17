#!/bin/sh
# PaperPro 拼音：打开平板的蓝牙（设置页里点“打开蓝牙”时运行）。
#
# reMarkable 没有在系统里启用蓝牙，但芯片（NXP IW61x）、驱动与固件都在：载入
# 驱动，等控制器出现，再启动系统自带的 bluetoothd。之后的开关、搜索、配对都由
# 输入法模块通过 D-Bus 交给 bluetoothd。
set -u
if [ ! -e /sys/class/bluetooth/hci0 ]; then
    modprobe bluetooth && modprobe btnxpuart || { echo "载入蓝牙驱动失败"; exit 1; }
fi
i=0
while [ ! -e /sys/class/bluetooth/hci0 ]; do
    i=$((i + 1))
    [ "$i" -gt 100 ] && { echo "没有找到蓝牙控制器"; exit 2; }
    sleep 0.2
done
systemctl start bluetooth.service || { echo "bluetoothd 启动失败"; exit 3; }
