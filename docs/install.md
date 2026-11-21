# 安装指南

## 准备

- 一台 reMarkable Paper Pro（已测试固件 3.30）或 Paper Pro Move（已测试固件 3.29）
- 一台电脑（macOS、Linux 或 Windows 10 及以上，都自带 `ssh` 与 `scp`）
- 一根 USB-C 数据线

## 第一步：开启开发者模式

PaperPro 拼音需要以 root 身份在平板上安装，这要求平板处于**开发者模式**。

> ⚠️ **开启开发者模式会清除平板上的全部数据（恢复出厂设置）**。开启前请确认笔记已同步到云端，或另行备份。已经开启过开发者模式的设备可以跳过这一步。

开启方法请参照 reMarkable 官方说明：<https://developer.remarkable.com/documentation/developer-mode>。

开启后，平板的设置里会显示 root 登录密码（官方说明中有具体位置），下面要用到。

## 第二步：下载发布包

在 [Releases](https://github.com/B4yesC4t/paperpro-pinyin/releases) 页面下载最新的 `paperpro-pinyin.tar.gz`，或者在终端里：

```sh
curl -LO https://github.com/B4yesC4t/paperpro-pinyin/releases/latest/download/paperpro-pinyin.tar.gz
```

可选：用同目录下的 `.sha256` 文件校验完整性。

## 第三步：复制到平板

用 USB 线连接平板与电脑，平板解锁后，电脑上会出现一个地址为 `10.11.99.1` 的网络连接。

```sh
scp paperpro-pinyin.tar.gz root@10.11.99.1:/home/root/
```

- 首次连接会询问是否信任主机，输入 `yes`；密码为上一步看到的 root 密码。
- 如果报 `subsystem request failed` 或 `Connection closed`，改用传统协议：`scp -O …`。

## 第四步：安装

```sh
ssh root@10.11.99.1
cd /home/root
tar xzf paperpro-pinyin.tar.gz
sh paperpro-pinyin/install.sh
```

安装脚本会：

1. 检查设备与系统；
2. 自检拼音引擎（`wojuede` → 我觉得）；
3. 在系统分区写入一个 xochitl 配置片段和一个定时同步服务（见下文“安装了什么”）；
4. 重启 xochitl，界面会重新载入一次。

看到“安装完成”即可。安装包约占用 `/home` 下 150 MB 空间，安装完成后可以删除 `paperpro-pinyin.tar.gz`。

### 已装有 xovi 等注入工具

如果 xochitl 已经通过 `LD_PRELOAD` 加载了其他工具（例如 xovi），安装脚本会提示并停止：两者只能生效一个。确认改用 PaperPro 拼音时运行：

```sh
sh paperpro-pinyin/install.sh --force
```

## 升级

下载新版本后，重复第三、四步即可。解压会覆盖旧文件，已学到的词（`user/` 目录）会保留。

## 安装了什么

| 位置 | 内容 |
| --- | --- |
| `/home/root/paperpro-pinyin/` | 输入法模块、Rime 引擎、词库、录音程序 `voice-hfp`、图标字体、脚本；`user/` 为学到的词和设置（含 DeepSeek API Key） |
| `/usr/lib/libpaperpro-pinyin-loader.so` | 加载器（约 70 KB）：等加密的 `/home` 就绪后加载输入法模块 |
| `/usr/lib/systemd/system/xochitl.service.d/zz-paperpro-pinyin.conf` | 让 xochitl 启动时预加载上面的加载器 |
| `/usr/lib/systemd/system/paperpro-pinyin-mirror.{service,timer}` | 每 10 分钟检查一次：系统更新安装完成后，把上面几个文件同步到新系统分区 |

系统分区平时是只读的，安装和卸载时会临时改为可写，写完立即恢复。

## 语音输入

安装后即可使用拼音。语音输入还需要在平板上打开蓝牙、配对麦克风、填写 DeepSeek API Key，
全部在“输入法设置”里完成，见 [语音输入](voice.md)。

## 卸载

```sh
sh /home/root/paperpro-pinyin/uninstall.sh           # 移除系统文件，保留安装目录
sh /home/root/paperpro-pinyin/uninstall.sh --purge   # 一并删除安装目录
```

卸载会同时清理已同步到另一个系统分区的文件。

## 系统更新之后

reMarkable 的系统更新会写入另一个系统分区，重启后切换过去。PaperPro 拼音的同步服务会在更新安装完成后，自动把配置复制到新分区，所以更新后无需重装。

如果更新是在关机状态下、或安装输入法之前就已下载完成的，可以在更新重启前手动同步一次：

```sh
sh /home/root/paperpro-pinyin/mirror-root.sh
```

更新后若输入法没有出现，重新运行一次 `install.sh` 即可。
