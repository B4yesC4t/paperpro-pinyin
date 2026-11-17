#!/bin/sh
# 构建 PaperPro 拼音的全部内容，输出到 out/paperpro-pinyin/（即安装包的内容）。
#
# 所有编译都在 arm64 的 Debian trixie 容器里进行（与平板同为 aarch64，glibc
# 不高于平板的 2.43）。需要 Docker；Apple Silicon 的 Mac 与 arm64 Linux 原生运行，
# x86_64 主机需要 qemu-user-static（binfmt）。
#
#   scripts/build.sh            全部构建
#   RIME_ICE_REV=<tag> ...      指定雾凇拼音的版本（默认见下）
#
# 产物：
#   libpaperpro-pinyin.so       注入 xochitl 的输入法模块（Qt 6.10）
#   libpaperpro-pinyin-loader.so  加载器：装在系统分区，等 /home 就绪后加载模块
#   voice-hfp                   蓝牙免提麦克风录音（语音输入）
#   rime/pinyin-rime            Rime 引擎的命令行助手
#   rime/lib, rime/plugins      librime 及其依赖、librime-lua
#   rime/shared                 雾凇拼音方案与预编译词库
#   licenses/                   第三方组件的许可证
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
VERSION=$(cat VERSION)
QT_VERSION=${QT_VERSION:-6.10.3}
RIME_ICE_REV=${RIME_ICE_REV:-2026.06.30}
OUT=out/paperpro-pinyin

rm -rf "$OUT"
mkdir -p "$OUT" .cache

docker run --rm --platform linux/arm64 \
    -e QT_VERSION="$QT_VERSION" -e RIME_ICE_REV="$RIME_ICE_REV" -e VERSION="$VERSION" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    -v "$ROOT:/w" debian:trixie sh -euc '
export DEBIAN_FRONTEND=noninteractive
echo "==> 安装构建依赖"
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends \
    g++ gcc libc6-dev libgl-dev libxkbcommon-dev git ca-certificates python3 python3-pip \
    librime1t64 librime-plugin-lua librime-bin librime-dev >/dev/null 2>&1

Q=/w/.cache/qt/$QT_VERSION/gcc_arm64
if [ ! -x "$Q/libexec/moc" ]; then
    echo "==> 下载 Qt $QT_VERSION（aqtinstall，仅首次）"
    pip3 install -q --break-system-packages aqtinstall >/dev/null
    python3 -m aqt install-qt linux_arm64 desktop "$QT_VERSION" linux_gcc_arm64 \
        --archives qtbase qtdeclarative --outputdir /w/.cache/qt >/dev/null
fi

OUT=/w/out/paperpro-pinyin
mkdir -p $OUT/rime/lib $OUT/rime/plugins $OUT/rime/shared $OUT/licenses /tmp/b

echo "==> 编译输入法模块"
M=/w/src/module
for h in bluetooth voice; do
    $Q/libexec/moc -I$Q/include -I$Q/include/QtCore $M/$h.h -o /tmp/b/moc_$h.cpp
done
$Q/libexec/moc -I$Q/include -I$Q/include/QtCore $M/paperpro-pinyin.cpp -o /tmp/b/paperpro-pinyin.moc
# overlay.qml 作为字符串常量编进模块。
{ printf "static const char kOverlayQml[] = R\"QML("; cat $M/overlay.qml; printf ")QML\";\n"; } > /tmp/b/overlay.qml.h
g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -I/tmp/b -I$M \
    -DPINYIN_VERSION="\"$VERSION\"" \
    -I$Q/include -I$Q/include/QtCore -I$Q/include/QtGui -I$Q/include/QtQml -I$Q/include/QtQuick \
    -I$Q/include/QtQmlIntegration -I$Q/include/QtNetwork -I$Q/include/QtDBus \
    $M/paperpro-pinyin.cpp -o $OUT/libpaperpro-pinyin.so \
    -L$Q/lib -lQt6Quick -lQt6Qml -lQt6Gui -lQt6Network -lQt6DBus -lQt6Core -Wl,--as-needed -Wl,-z,defs
strip --strip-unneeded $OUT/libpaperpro-pinyin.so

echo "==> 编译加载器"
# 带上与模块相同的 Qt 库（--no-as-needed），使加载器在 Qt 初始化之后才运行。
gcc -O2 -fPIC -shared -Wall -Wextra /w/src/loader/paperpro-pinyin-loader.c -o $OUT/libpaperpro-pinyin-loader.so \
    -L$Q/lib -Wl,--no-as-needed -lQt6Quick -lQt6Qml -lQt6Gui -lQt6Core -Wl,--as-needed -ldl -lpthread
strip --strip-unneeded $OUT/libpaperpro-pinyin-loader.so

echo "==> 编译蓝牙麦克风录音程序"
gcc -O2 -Wall -Wextra -o $OUT/voice-hfp /w/src/helper/voice-hfp.c
strip $OUT/voice-hfp

echo "==> 编译 Rime 助手"
gcc -O2 -Wall -o $OUT/rime/pinyin-rime /w/src/helper/pinyin-rime.c -lrime -ldl
strip $OUT/rime/pinyin-rime
cp /usr/lib/aarch64-linux-gnu/rime-plugins/librime-lua.so $OUT/rime/plugins/

echo "==> 获取雾凇拼音 $RIME_ICE_REV 并预编译词库"
git -c advice.detachedHead=false clone -q --depth 1 --branch "$RIME_ICE_REV" https://github.com/iDvel/rime-ice.git /tmp/ice
cp -R /tmp/ice/* $OUT/rime/shared/
cp /tmp/ice/LICENSE $OUT/licenses/rime-ice.LICENSE
rm -rf $OUT/rime/shared/others $OUT/rime/shared/.github $OUT/rime/shared/*.md $OUT/rime/shared/LICENSE
# 在这里编译方案和词库：平板的 CPU 上要好几分钟。
mkdir -p /tmp/user
(cd $OUT/rime/shared && rime_deployer --build /tmp/user $OUT/rime/shared $OUT/rime/shared/build) \
    >/tmp/deploy.log 2>&1 || { tail -n 20 /tmp/deploy.log; exit 1; }

echo "==> 收集运行库（平板自带的 C 运行库除外）"
for f in $OUT/rime/pinyin-rime $OUT/rime/plugins/*.so; do ldd "$f"; done | awk "/=> \\//{print \$3}" | sort -u \
  | grep -v -E "/(libc|libm|libdl|libpthread|librt|libresolv|ld-linux-aarch64)\.so" > /tmp/libs
while read -r l; do
    cp -L "$l" $OUT/rime/lib/
    pkg=$(dpkg -S "$(readlink -f "$l")" 2>/dev/null | head -n 1 | cut -d: -f1) || true
    [ -n "$pkg" ] && [ -f /usr/share/doc/$pkg/copyright ] && cp /usr/share/doc/$pkg/copyright $OUT/licenses/$pkg.copyright
done < /tmp/libs
for pkg in librime1t64 librime-plugin-lua; do
    cp /usr/share/doc/$pkg/copyright $OUT/licenses/$pkg.copyright
done
missing=$(LD_LIBRARY_PATH=$OUT/rime/lib ldd $OUT/rime/pinyin-rime $OUT/rime/plugins/librime-lua.so | grep "not found" || true)
[ -z "$missing" ] || { echo "缺少运行库：$missing"; exit 1; }

echo "==> 自检：wojuede"
got=$(printf "key 119 0\nkey 111 0\nkey 106 0\nkey 117 0\nkey 101 0\nkey 100 0\nkey 101 0\n" |
      LD_LIBRARY_PATH=$OUT/rime/lib $OUT/rime/pinyin-rime $OUT/rime/shared /tmp/user $OUT/rime/plugins/librime-lua.so 2>/dev/null | tail -n 1)
echo "$got" | grep -q "我觉得" || { echo "自检失败：$got"; exit 1; }
echo "    $got" | cut -c1-120

chown -R "$HOST_UID:$HOST_GID" /w/out /w/.cache
'

cp device/install.sh device/uninstall.sh device/mirror-root.sh device/bluetooth-up.sh "$OUT/"
chmod +x "$OUT"/*.sh
cp LICENSE THIRD_PARTY_NOTICES.md VERSION "$OUT/"
cp assets/paperpro-pinyin-icons.ttf "$OUT/"
cp assets/MaterialIcons.LICENSE "$OUT/licenses/material-icons.LICENSE"
du -sh "$OUT"
echo "构建完成：$OUT"
