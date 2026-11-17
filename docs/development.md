# 从源码构建

## 环境

- Docker（构建在 arm64 的 Debian trixie 容器中进行）
  - Apple Silicon Mac、arm64 Linux：原生运行
  - x86_64 主机：需要先启用 qemu 用户态模拟，例如 `docker run --privileged --rm tonistiigi/binfmt --install arm64`
- 网络：首次构建会下载 Qt 6.10.3 SDK（aqtinstall，缓存在 `.cache/qt`）和雾凇拼音

## 构建

```sh
make            # 等同于 scripts/build.sh：输出 out/paperpro-pinyin/
make package    # 打包为 dist/paperpro-pinyin.tar.gz 及 .sha256
```

可用的环境变量：

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `QT_VERSION` | `6.10.3` | 编译时使用的 Qt 头文件版本，应与平板固件的 Qt 一致 |
| `RIME_ICE_REV` | 见 `scripts/build.sh` | 雾凇拼音的 Git 标签 |

构建脚本最后会自检：向引擎输入 `wojuede`，结果须为“我觉得”。

## 目录结构

```
src/module/paperpro-pinyin.cpp   注入 xochitl 的 Qt 模块：事件过滤、候选栏、光标定位、键盘布局与按键
src/module/overlay.qml           候选栏、语音状态、设置页（构建时编为字符串常量）
src/module/bluetooth.h           蓝牙：通过 D-Bus 控制 bluetoothd（开关、搜索、配对）
src/module/voice.h               语音输入：录音子进程、DeepSeek 识别与 Flash 整理
src/loader/paperpro-pinyin-loader.c  预加载的加载器：等 /home 就绪后打开模块
src/helper/pinyin-rime.c         librime 的按行协议包装（子进程）
src/helper/voice-hfp.c           蓝牙免提麦克风录音：用户态 RFCOMM + HFP 音频网关 + SCO
device/bluetooth-up.sh           载入蓝牙驱动、启动 bluetoothd
assets/paperpro-pinyin-icons.ttf 屏幕键盘图标（Material Icons 子集）
device/install.sh                在平板上安装
device/uninstall.sh              在平板上卸载
device/mirror-root.sh            系统更新后同步配置到新分区
scripts/build.sh                 构建
scripts/package.sh               打包
docs/                            文档
```

## 部署到开发设备

```sh
make package
scp dist/paperpro-pinyin.tar.gz root@10.11.99.1:/home/root/
ssh root@10.11.99.1 'cd /home/root && tar xzf paperpro-pinyin.tar.gz && sh paperpro-pinyin/install.sh'
```

只改了模块时，可以只替换库文件（先改名再移动，避免覆盖正在使用的文件）：

```sh
cat out/paperpro-pinyin/libpaperpro-pinyin.so | ssh root@10.11.99.1 \
  'cat > /home/root/paperpro-pinyin/lib.new && mv /home/root/paperpro-pinyin/lib.new /home/root/paperpro-pinyin/libpaperpro-pinyin.so \
   && systemctl reset-failed xochitl && systemctl restart xochitl'
```

> 注意：xochitl 在 10 分钟内失败 4 次会进入紧急模式。频繁重启时请先 `systemctl reset-failed xochitl`。

## 调试

在安装目录创建 `debug` 文件并重启 xochitl 后，模块会：

- 把按键、输入法事件、光标换算等记入日志（`journalctl -u xochitl -f | grep paperpro-pinyin`）；
- 创建控制管道 `/home/root/paperpro-pinyin/ctl`，可在 SSH 中远程操作：

| 命令 | 作用 |
| --- | --- |
| `echo "type wo" > ctl` | 像键盘输入一样把字母交给引擎 |
| `echo "key 65307" > ctl` | 发送一个 keysym（例：Esc） |
| `echo 'commit 第一行\n第二行' > ctl` | 把文字直接插入光标处（`\n` 为换行），像语音结果上屏一样 |
| `echo dump > ctl` | 记录焦点对象、光标、图层结构与候选栏状态 |
| `echo grab > ctl` | 把当前界面截图保存为 `/tmp/paperpro-pinyin-grab.png` |
| `echo "prop keyboardVisibility" > ctl` | 读取焦点对象（或其上层）的属性 |
| `echo "call showVirtualKeyboard" > ctl` | 调用焦点对象（或其上层）的方法 |
| `echo settings > ctl` / `echo "settings off" > ctl` | 打开 / 关闭设置页 |
| `echo "bt on" > ctl` / `bt off` / `bt scan` | 蓝牙开关、搜索 |
| `echo "voice test" > ctl` / `echo state > ctl` | DeepSeek 连接测试；蓝牙与语音状态 |
| `echo "tree <类名正则> [深度]" > ctl` | 列出界面元素及其子元素（找 xochitl 的界面结构） |
| `echo "methods <正则>" > ctl` | 列出名字匹配的方法与属性 |
| `echo "probe import QtQuick; …" > ctl` | 在 xochitl 的 QML 引擎里创建对象，列出其属性与枚举 |
| `echo modes > ctl` / `echo "fastmode <n>" > ctl` | 列出墨水屏刷新区域；切换候选栏的刷新方式 |
| `echo "res <路径>" > ctl` | 把资源文件（如 `:/misc/keyboards/…`）复制到 `/tmp/paperpro-pinyin-res` |

调试模式会记录按键内容，用完请删除 `debug` 文件并重启 xochitl。

单独测试蓝牙麦克风录音（录 5 秒，输出 8 kHz 16 位单声道 PCM）：

```sh
sleep 30 | /home/root/paperpro-pinyin/voice-hfp <麦克风地址> <RFCOMM 通道> 5 > /tmp/v.pcm
```

## 发布

推送形如 `v0.1.0` 的标签后，GitHub Actions 会在 arm64 运行器上构建并创建 Release，附上安装包。
