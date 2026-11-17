# 常见问题

### 打字没有反应，也没有候选栏

1. 确认处于中文模式：外接键盘上单击一次 Shift（或 Ctrl+空格），屏幕键盘上点「中/英」，光标旁会短暂显示“中”或“英”。
2. 确认模块已加载：

   ```sh
   journalctl -u xochitl | grep paperpro-pinyin
   ```

   应能看到 `loaded (version …)`。如果看到 `stood down`，说明本次开机内 xochitl 连续异常退出过，模块已自动停用，重启平板即可再次尝试；`disabled by …` 则表示存在 `disabled` 文件。
3. 若什么都没有，检查配置是否存在：`systemctl cat xochitl | grep LD_PRELOAD`，应指向 `/usr/lib/libpaperpro-pinyin-loader.so`。不存在或指向别处时重新运行 `install.sh`。

### 屏幕键盘弹出来是空白的

这是装过其他中文键盘插件后，键盘语言停留在 `zh_CN` 造成的（日志中有 `Failed to open layout file ":/misc/keyboards/zh_CN/keyboard_layout.json"`）。PaperPro 拼音启动时会自动补上这个布局，日志中应能看到：

```
[paperpro-pinyin] keyboard: en_US layout offered as zh_CN: ok
```

也可以在平板的“设置 → 语言”中把键盘布局改回英文，PaperPro 拼音在任何英文布局下都能工作。

### 系统更新到 3.30.0.111 后，候选栏一顿一顿

这是目前已知的问题：在 3.30.0.111 上，浮在笔记上的候选栏每变一次都要等一次较慢的
墨水屏刷新，打字快时会排队，打完还要等一两秒。同一个输入法在 3.30.0.108 上很流畅。
我们还在寻找在新固件上的解决办法。

如果更新前用的是 3.30.0.108，旧系统仍完整保存在另一个系统分区里，可以切回去
（与系统自己的 A/B 回退是同一机制，随时可以再切回来）：

```sh
rootdev --active       # 当前系统分区
rootdev --switch       # 下次启动改用另一个分区
reboot
```

切回旧系统后，平板会再次自动下载新版本，并在重启时切换过去（设置里的“自动更新”
关掉也会如此）。介意的话请自行权衡是否停用系统更新服务；停用后将收不到任何系统更新。

### 屏幕键盘上怎么切换中英、打开设置

中文屏幕键盘最下面一排是：🌐 · 123 · ⚙ · 🎤 · ， · 空格 · 。 · 中/英 · 回车。
「中/英」切换中英文，⚙ 打开输入法设置，🎤 开始/结束语音输入。屏幕键盘的 Shift
（包括句首自动大写）只管大小写，不切换中英；中文模式下大写字母照样当拼音。

### Type Folio 接上后屏幕键盘不出现

这是系统的正常行为：接有实体键盘时，xochitl 不显示屏幕键盘。拔下 Type Folio 即可。

### 候选栏出现在屏幕顶部，而不是光标下方

模块没能取得光标位置时，会把候选栏放在屏幕顶部居中，保证可用。这通常发生在
笔记本以外的输入框，或固件更新改变了笔记编辑器的内部接口。欢迎提交 Issue，并附上
固件版本（设置 → 关于）。

### 能和 xovi、AppLoad 等工具共存吗

xovi 等工具同样通过 `LD_PRELOAD` 注入 xochitl。systemd 的环境变量后设置的会覆盖先设置的，所以两者不能同时生效。安装脚本检测到时会提示，可用 `--force` 选择 PaperPro 拼音。

### 系统更新后输入法没了

正常情况下更新会被自动同步。如果更新后没有出现，运行：

```sh
sh /home/root/paperpro-pinyin/install.sh
```

### 学到的词保存在哪里

`/home/root/paperpro-pinyin/user/`。升级时会保留；`uninstall.sh --purge` 会删除它。

### 会上传我输入的内容吗

打拼音不会：引擎和词库都在平板本地运行。语音输入会把录音上传到 DeepSeek 识别，
开启 Flash 优化时再把识别出的文字发给 DeepSeek 整理；不用语音输入就没有任何网络请求。
详见 [语音输入](voice.md#隐私)。

### 语音输入的问题

见 [语音输入 · 常见问题](voice.md#常见问题)。

### 如何反馈问题

请到 [Issues](https://github.com/B4yesC4t/paperpro-pinyin/issues) 提交，并附上：

- 固件版本（设置 → 关于）
- `journalctl -u xochitl -b | grep paperpro-pinyin` 的输出

需要更详细的日志时，可以临时开启调试（会把按键记入系统日志，用完请关闭）：

```sh
touch /home/root/paperpro-pinyin/debug && systemctl restart xochitl
# 复现问题后
journalctl -u xochitl -b | grep paperpro-pinyin > pinyin.log
rm /home/root/paperpro-pinyin/debug && systemctl restart xochitl
```
