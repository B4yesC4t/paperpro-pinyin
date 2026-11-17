#!/bin/sh
# PaperPro 拼音：安装（在平板上以 root 运行）。
#
#   sh install.sh            安装或升级，并重启 xochitl
#   sh install.sh --force    已有其他 LD_PRELOAD 注入工具（如 xovi）时也照样安装
#
# 做的事：
#   1. 把本目录复制到 /home/root/paperpro-pinyin（保留已学到的词：user/）
#   2. 自检 Rime 引擎（输入 wojuede 应得到“我觉得”）
#   3. 在系统分区写入几个小文件（/ 需临时改为可写）：
#        /usr/lib/libpaperpro-pinyin-loader.so        加载器：等加密的 /home 就绪后加载模块
#        xochitl.service.d/zz-paperpro-pinyin.conf   让 xochitl 预加载这个加载器
#        paperpro-pinyin-mirror.service / .timer       系统更新后把上述文件同步到新分区
#   4. 重启 xochitl
set -eu
DEST=/home/root/paperpro-pinyin
SRC=$(cd "$(dirname "$0")" && pwd)
UNITS=/usr/lib/systemd/system
DROPIN=$UNITS/xochitl.service.d/zz-paperpro-pinyin.conf
LOADER=/usr/lib/libpaperpro-pinyin-loader.so
FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

say() { printf '%s\n' "$*"; }
die() { printf '安装失败：%s\n' "$*" >&2; exit 1; }

# ---- 检查 ----------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "请以 root 运行"
[ "$(uname -m)" = aarch64 ] || die "只支持 reMarkable Paper Pro（aarch64），这台是 $(uname -m)"
[ -x /usr/bin/xochitl ] || die "找不到 /usr/bin/xochitl"
[ -e /usr/lib/libQt6Core.so.6 ] || die "系统里没有 Qt 6，固件版本可能太旧（需要 3.20 以上，已在 3.30 测试）"
for f in libpaperpro-pinyin.so libpaperpro-pinyin-loader.so; do
    [ -f "$SRC/$f" ] || die "安装包不完整：缺少 $f"
done

other=$(systemctl show xochitl -p Environment | tr ' ' '\n' | sed -n 's/^LD_PRELOAD=//p' | grep -v paperpro-pinyin || true)
if [ -n "$other" ] && [ "$FORCE" = 0 ]; then
    die "xochitl 已经通过 LD_PRELOAD 加载了 $other。
两者不能同时生效（后安装的会覆盖先安装的）。确认要改用 PaperPro 拼音，请运行：sh install.sh --force"
fi

# ---- 复制 ----------------------------------------------------------------------
if [ "$SRC" != "$DEST" ]; then
    say "==> 复制到 $DEST"
    rm -rf "$DEST.new"
    cp -a "$SRC" "$DEST.new"
    [ -d "$DEST/user" ] && rm -rf "$DEST.new/user" && mv "$DEST/user" "$DEST.new/user"
    rm -rf "$DEST.old"
    [ -d "$DEST" ] && mv "$DEST" "$DEST.old"
    mv "$DEST.new" "$DEST"
    rm -rf "$DEST.old"
fi
mkdir -p "$DEST/user"
chmod +x "$DEST/rime/pinyin-rime" "$DEST/voice-hfp" "$DEST"/*.sh

# ---- 自检 ----------------------------------------------------------------------
say "==> 自检 Rime 引擎"
T=$(mktemp -d)
got=$(printf 'key 119 0\nkey 111 0\nkey 106 0\nkey 117 0\nkey 101 0\nkey 100 0\nkey 101 0\n' |
      LD_LIBRARY_PATH="$DEST/rime/lib" "$DEST/rime/pinyin-rime" "$DEST/rime/shared" "$T" \
      "$DEST/rime/plugins/librime-lua.so" 2>/dev/null | tail -n 1)
rm -rf "$T"
case "$got" in
*我觉得*) say "    wojuede → 我觉得" ;;
*) die "Rime 引擎没有正常工作：$got" ;;
esac

# ---- 系统分区 ------------------------------------------------------------------
say "==> 写入系统文件"
mount -o remount,rw /
mkdir -p "$(dirname "$DROPIN")" "$UNITS/timers.target.wants"
# 先写到新文件再改名：正在运行的 xochitl 映射着旧的加载器。
cp "$DEST/libpaperpro-pinyin-loader.so" "$LOADER.new" && mv "$LOADER.new" "$LOADER"
printf '[Service]\nEnvironment=LD_PRELOAD=%s\n' "$LOADER" > "$DROPIN"
cat > "$UNITS/paperpro-pinyin-mirror.service" <<EOF
[Unit]
Description=PaperPro 拼音：系统更新后把配置同步到新系统分区
ConditionPathExists=$DEST/mirror-root.sh

[Service]
Type=oneshot
ExecStart=/bin/sh $DEST/mirror-root.sh
EOF
cat > "$UNITS/paperpro-pinyin-mirror.timer" <<'EOF'
[Unit]
Description=PaperPro 拼音：定期检查系统更新

[Timer]
OnBootSec=3min
OnUnitActiveSec=10min

[Install]
WantedBy=timers.target
EOF
ln -sf ../paperpro-pinyin-mirror.timer "$UNITS/timers.target.wants/paperpro-pinyin-mirror.timer"
sync
mount -o remount,ro / 2>/dev/null || say "    （/ 会保持可写直到下次重启，不影响使用）"
systemctl daemon-reload
systemctl start paperpro-pinyin-mirror.timer
# 已下载、等待重启切换的系统更新，也立即补上。
sh "$DEST/mirror-root.sh" || true

# ---- 启用 ----------------------------------------------------------------------
say "==> 重启 xochitl"
rm -f "$DEST/xochitl-starts"
systemctl reset-failed xochitl.service 2>/dev/null || true
systemctl restart xochitl.service
sleep 5
if journalctl -u xochitl --since "-20s" -o cat --no-pager | grep -q "\[paperpro-pinyin\] loaded"; then
    say ""
    say "安装完成。打开笔记本进入文字模式即可输入中文："
    say "  外接键盘单击 Shift 或 Ctrl+空格、屏幕键盘点「中/英」切换中英；数字键或点击选字。"
    say "  语音输入：在屏幕键盘上点 ⚙ 打开设置，打开蓝牙、配对麦克风、填写 DeepSeek API Key，"
    say "  之后按住空格说话即可（详见 docs/voice.md）。"
else
    say "已安装，但还没在日志里看到模块加载。可运行：journalctl -u xochitl | grep paperpro-pinyin"
fi
