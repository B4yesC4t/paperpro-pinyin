<div align="center">

# PaperPro 拼音

**reMarkable Paper Pro 原生系统里的中文拼音输入法，带语音输入**

在笔记本的文字模式里直接打中文：外接 Type Folio 和屏幕键盘都能用，候选栏出现在光标下方，和电脑上的输入法一样。接上蓝牙麦克风，按住空格就能说话输入。

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
![Device](https://img.shields.io/badge/设备-reMarkable%20Paper%20Pro-black)
![Firmware](https://img.shields.io/badge/已测试固件-3.30.0.108-black)
![Engine](https://img.shields.io/badge/引擎-Rime%20%2B%20雾凇拼音-black)

[安装](#安装) · [使用](#使用) · [语音输入](docs/voice.md) · [常见问题](docs/faq.md) · [工作原理](docs/how-it-works.md) · [从源码构建](docs/development.md)

</div>

---

## 特性

- **整句输入**：基于 [Rime](https://rime.im) 引擎和 [雾凇拼音](https://github.com/iDvel/rime-ice) 词库，`wojuede` → 我觉得，`nihaoma` → 你好吗；支持简拼、模糊音和词频学习。
- **原生体验**：候选栏是 xochitl 界面的一部分，由系统自己的墨水屏刷新机制绘制，快速且无残影；横屏、竖屏都跟着光标走。
- **两种键盘**：Type Folio 外接键盘与系统屏幕键盘都能输入中文；中文屏幕键盘多了「中/英」、设置和麦克风键。
- **语音输入**：按住空格说话，松开即插入文字。蓝牙麦克风录音，DeepSeek 识别，再由 DeepSeek Flash 修正错别字和标点、整理成分段清楚的笔记（可关闭，提示词与热词可自定义）。详见 [语音输入](docs/voice.md)。
- **拼音完全离线**：词库和引擎都在平板上运行。只有语音输入会联网（录音上传到 DeepSeek）。
- **重启和系统更新后依然可用**：开机自动加载；系统更新（A/B 分区）安装后，会自动把配置同步到新系统。
- **安全兜底**：如果 xochitl 加载后连续异常退出，输入法会在本次开机内自动停用，不会让平板陷入重启循环。
- **不改动系统程序**：不修改 xochitl，也不复制 reMarkable 的任何文件；卸载后系统恢复原样。

## 兼容性

| 设备 | 固件 | 状态 |
| --- | --- | --- |
| reMarkable Paper Pro | 3.30.0.108 | ✅ 已测试 |
| reMarkable Paper Pro | 3.30.0.111 | ⚠️ 可用，但候选栏刷新明显变慢（[说明](docs/faq.md#系统更新到-3300111-后候选栏一顿一顿)） |
| reMarkable Paper Pro | 其他 3.2x / 3.3x | 理论可用（未测试） |
| reMarkable Paper Pro Move | — | 未测试 |
| reMarkable 1 / 2 | — | ❌ 不支持（处理器架构与系统不同） |

> 输入法只依赖 Qt 的公开接口，并在运行时按名称查找界面对象；固件更新若改动了这些部分，最坏情况是候选栏位置退回屏幕顶部，或模块自动停用，不会影响系统本身。

## 安装

需要先在平板上开启**开发者模式**，以便通过 USB 以 root 登录。详细步骤（含开启开发者模式的注意事项）见 **[安装指南](docs/install.md)**。

简要流程：

```sh
# 1. 在电脑上下载发布包（或到 Releases 页面手动下载）
curl -LO https://github.com/B4yesC4t/paperpro-pinyin/releases/latest/download/paperpro-pinyin.tar.gz

# 2. 用 USB 线连接平板，复制到平板
scp paperpro-pinyin.tar.gz root@10.11.99.1:/home/root/

# 3. 登录平板并安装
ssh root@10.11.99.1
cd /home/root && tar xzf paperpro-pinyin.tar.gz && sh paperpro-pinyin/install.sh
```

安装脚本会自检引擎（输入 `wojuede` 应得到“我觉得”），写入开机配置，并重启 xochitl（界面会重新载入一次）。

## 使用

打开任意笔记本，切到文字模式（工具栏上的 **T**），就可以打字。

| 操作 | 按键 |
| --- | --- |
| 切换中 / 英 | 单击 **Shift**，或 **Ctrl + 空格**（切换时光标旁会短暂显示“中/英”） |
| 选择候选 | 数字键 **1–5**，或直接点击候选字 |
| 上屏第一个候选 | **空格** |
| 翻页 | **-** / **=**，或点击候选栏右侧的 ‹ › |
| 上屏输入的字母 | **回车** |
| 取消输入 | **Esc** |
| 中文标点 | 中文模式下 `,` `.` 等直接输出“，”“。” |
| 语音输入 | **按住空格**说话，松开即识别并插入 |

屏幕键盘与 Type Folio 用法相同：点字母打拼音，点候选上屏。中文屏幕键盘最下面一排是：

🌐 · 123 · **⚙ 设置** · **🎤 语音** · ， · 空格 · 。 · **中/英** · 回车

- **中/英**：切换中文与英文（屏幕键盘的 Shift 只管大小写，不切换中英）
- **🎤**：点一下开始说话，再点一下结束；也可以长按空格
- **⚙**：打开“输入法设置”（蓝牙麦克风、DeepSeek API Key、Flash 优化的提示词与热词）

## 卸载

```sh
ssh root@10.11.99.1
sh /home/root/paperpro-pinyin/uninstall.sh           # 保留安装目录（含已学到的词）
sh /home/root/paperpro-pinyin/uninstall.sh --purge   # 全部删除
```

临时停用而不卸载：`touch /home/root/paperpro-pinyin/disabled && systemctl restart xochitl`，删除该文件并重启 xochitl 即恢复。

## 文档

- [安装指南](docs/install.md)：开发者模式、复制文件、安装、升级
- [语音输入](docs/voice.md)：蓝牙麦克风、API Key、使用方法、隐私
- [常见问题](docs/faq.md)：候选栏不出现、屏幕键盘空白、系统更新后变慢、与 xovi 等工具共存……
- [工作原理](docs/how-it-works.md)：如何接入 xochitl、如何找到光标、为什么刷新快
- [从源码构建](docs/development.md)：构建环境、目录结构、调试方法

## 致谢

- [Rime / librime](https://github.com/rime/librime)：开源输入法引擎
- [雾凇拼音](https://github.com/iDvel/rime-ice)：优秀的简体拼音方案和词库
- [DeepSeek](https://www.deepseek.com)：语音识别与文字整理
- [Material Icons](https://github.com/google/material-design-icons)：屏幕键盘上的设置与麦克风图标
- 语音输入的交互参考了 Handy、OpenWhispr、VoiceInk 等开源语音输入工具
- reMarkable 社区里各种插件与研究带来的启发

## 许可证

本项目以 [GPL-3.0](LICENSE) 发布。发布包附带的第三方组件及其许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

本项目与 reMarkable AS 无关。修改设备系统存在风险，请在理解安装内容后使用，后果自负。
