# 第三方组件

PaperPro 拼音本身以 [GPL-3.0](LICENSE) 发布。发布包里还带有以下第三方组件，
各自遵循其原有许可证；完整的许可证文本在发布包的 `licenses/` 目录中。

| 组件 | 用途 | 许可证 | 来源 |
| --- | --- | --- | --- |
| [librime](https://github.com/rime/librime) | 输入法引擎 | BSD-3-Clause | Debian trixie `librime1t64` |
| [librime-lua](https://github.com/hchunhui/librime-lua) | 方案中的 Lua 扩展 | BSD-3-Clause | Debian trixie `librime-plugin-lua` |
| [雾凇拼音 rime-ice](https://github.com/iDvel/rime-ice) | 拼音方案与词库 | GPL-3.0 | GitHub 发布标签 |
| [OpenCC](https://github.com/BYVoid/OpenCC) | 简繁转换 | Apache-2.0 | Debian trixie |
| Boost、glog、gflags、yaml-cpp、LevelDB、marisa-trie、Lua 等 | librime 的依赖库 | 各自许可证（见 `licenses/`） | Debian trixie |
| [Material Icons](https://github.com/google/material-design-icons)（Outlined 子集：settings、mic、backspace、keyboard_return、keyboard_tab、keyboard_hide、keyboard_capslock、arrow_upward、chevron_left、chevron_right） | 屏幕键盘的图标 | Apache-2.0 | Google 官方仓库，`paperpro-pinyin-icons.otf` |

输入法模块在运行时动态链接平板系统自带的 Qt 6（LGPL-3.0），本项目不分发 Qt。

语音输入调用 DeepSeek 的在线服务（语音识别与 `deepseek-flash` 模型），需要用户自己的
API Key；使用时受 DeepSeek 服务条款约束。本项目不附带任何 Key。

本项目与 reMarkable AS 无关，不包含 reMarkable 的任何代码或数据。屏幕键盘的
中文布局在运行时引用平板系统自带的英文布局，不复制其文件。
