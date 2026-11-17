#!/bin/sh
# PaperPro 拼音：把系统分区里的配置同步到另一个系统分区。
#
# reMarkable 的系统更新是 A/B 方式：更新写入当前未使用的分区，下次启动切换
# 过去，而那个分区里没有我们加的文件，输入法就会在更新后消失。本脚本由
# paperpro-pinyin-mirror.timer 每 10 分钟运行一次：另一个分区里是完整的系统、
# 且没有正在写入的更新时，把同样的文件复制过去（已一致则什么也不做）。
set -u
U=usr/lib/systemd/system
FILES="usr/lib/libpaperpro-pinyin-loader.so
$U/xochitl.service.d/zz-paperpro-pinyin.conf
$U/paperpro-pinyin-mirror.service
$U/paperpro-pinyin-mirror.timer
$U/timers.target.wants/paperpro-pinyin-mirror.timer"

cur=$(sed -n 's/.*root=\(\/dev\/mmcblk0p[0-9]*\).*/\1/p' /proc/cmdline)
case "$cur" in
/dev/mmcblk0p2) other=/dev/mmcblk0p3 ;;
/dev/mmcblk0p3) other=/dev/mmcblk0p2 ;;
*) exit 0 ;;
esac
# 只同步本系统自己装着的（已卸载就不再同步）。
for f in $FILES; do [ -e "/$f" ] || exit 0; done

# 更新正在写入时不动：更新服务 3 分钟内没有动静，且那个分区 5 秒内没有写入。
recent=$(journalctl -u swupdate -u update-engine --since "-3min" -o cat --no-pager 2>/dev/null | wc -l)
[ "$recent" -eq 0 ] || exit 0
stat=/sys/class/block/${other#/dev/}/stat
written() { awk '{print $7}' "$stat"; }
w1=$(written) && sleep 5 && w2=$(written) || exit 0
[ "$w1" = "$w2" ] || exit 0

# 每次用独立的挂载点：定时器和安装脚本可能同时运行。
M=$(mktemp -d /tmp/paperpro-pinyin-root.XXXXXX)
trap 'umount "$M" 2>/dev/null; rmdir "$M" 2>/dev/null' EXIT
mount -o ro "$other" "$M" 2>/dev/null || exit 0
[ -f "$M/etc/os-release" ] || exit 0
need=0
for f in $FILES; do cmp -s "/$f" "$M/$f" || need=1; done
if [ "$need" = 1 ] && mount -o remount,rw "$M"; then
    for f in $FILES; do
        mkdir -p "$(dirname "$M/$f")"
        if [ -L "/$f" ]; then
            ln -sf "$(readlink "/$f")" "$M/$f"
        else
            cp "/$f" "$M/$f"
        fi
    done
    sync
    echo "paperpro-pinyin: 已同步到 $other（$(sed -n 's/^IMG_VERSION=//p' "$M/etc/os-release")）"
fi
