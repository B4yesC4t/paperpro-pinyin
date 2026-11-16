#!/bin/sh
# 把 out/paperpro-pinyin/ 打成发布包：dist/paperpro-pinyin.tar.gz（附 .sha256）。
# 文件名不带版本号，这样 releases/latest/download/paperpro-pinyin.tar.gz 始终指向最新版；
# 版本号写在包内的 VERSION 文件里。
set -eu
cd "$(dirname "$0")/.."
[ -f out/paperpro-pinyin/libpaperpro-pinyin.so ] || { echo "请先运行 scripts/build.sh"; exit 1; }
mkdir -p dist
NAME=paperpro-pinyin.tar.gz
# COPYFILE_DISABLE：macOS 的 tar 不附带 ._ 元数据文件。
if tar --version 2>/dev/null | grep -q GNU; then
    tar -C out -czf "dist/$NAME" --owner=0 --group=0 paperpro-pinyin
else
    COPYFILE_DISABLE=1 tar -C out -czf "dist/$NAME" --uid 0 --gid 0 paperpro-pinyin
fi
(cd dist && { sha256sum "$NAME" 2>/dev/null || shasum -a 256 "$NAME"; } > "$NAME.sha256")
echo "版本 $(cat VERSION)"
ls -la dist
