#!/bin/sh
# PaperPro 拼音：卸载（在平板上以 root 运行）。
#
#   sh /home/root/paperpro-pinyin/uninstall.sh           移除系统文件，保留安装目录（含已学到的词）
#   sh /home/root/paperpro-pinyin/uninstall.sh --purge   连同 /home/root/paperpro-pinyin 一起删除
set -u
DEST=/home/root/paperpro-pinyin
UNITS=/usr/lib/systemd/system

FILES="/usr/lib/libpaperpro-pinyin-loader.so
$UNITS/xochitl.service.d/zz-paperpro-pinyin.conf
$UNITS/paperpro-pinyin-mirror.service
$UNITS/paperpro-pinyin-mirror.timer
$UNITS/timers.target.wants/paperpro-pinyin-mirror.timer"

systemctl stop paperpro-pinyin-mirror.timer 2>/dev/null
mount -o remount,rw / && rm -f $FILES && sync
mount -o remount,ro / 2>/dev/null

# 另一个系统分区里同步过去的副本（已下载、等待重启的系统更新）也一并移除。
cur=$(sed -n 's/.*root=\(\/dev\/mmcblk0p[0-9]*\).*/\1/p' /proc/cmdline)
case "$cur" in
/dev/mmcblk0p2) other=/dev/mmcblk0p3 ;;
/dev/mmcblk0p3) other=/dev/mmcblk0p2 ;;
*) other= ;;
esac
M=$(mktemp -d /tmp/paperpro-pinyin-root.XXXXXX)
if [ -n "$other" ] && mount -o rw "$other" "$M" 2>/dev/null; then
    [ -f "$M/etc/os-release" ] && for f in $FILES; do rm -f "$M$f"; done
    sync
    umount "$M"
fi
rmdir "$M"
systemctl daemon-reload
systemctl reset-failed xochitl.service 2>/dev/null
systemctl restart xochitl.service
if [ "${1:-}" = "--purge" ]; then
    rm -rf "$DEST"
    echo "已卸载，并删除了 $DEST"
else
    echo "已卸载。安装目录 $DEST 仍保留（重新安装：sh $DEST/install.sh；彻底删除：加 --purge）"
fi
