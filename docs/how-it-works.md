# 工作原理

PaperPro 拼音由两部分组成：

```
┌──────────────────────── xochitl（reMarkable 的界面程序）─────────────────────────┐
│                                                                                │
│  Type Folio 按键 ─┐                                                             │
│                   ├─▶ libpaperpro-pinyin.so ──QInputMethodEvent──▶ 笔记编辑器   │
│  屏幕键盘输入  ───┘     （事件过滤器 + 候选栏）                                  │
│                              │   ▲                                             │
└──────────────────────────────┼───┼─────────────────────────────────────────────┘
                     按键/选字  ▼   │  候选、组字、上屏文字（JSON）
                         rime/pinyin-rime（子进程：librime + 雾凇拼音）
```

## 1. 接入 xochitl：LD_PRELOAD + 事件过滤器

xochitl 是 Qt 6 / QML 程序。systemd 配置片段 `zz-paperpro-pinyin.conf` 为它设置
`LD_PRELOAD`，让它启动时加载 `libpaperpro-pinyin.so`。模块通过
`Q_COREAPP_STARTUP_FUNCTION` 在应用对象创建时运行，安装一个**应用级事件过滤器**：

- **Type Folio**：硬件按键先到达窗口。有输入焦点且处于中文模式时，字母交给 Rime，事件本身被吞掉，不会打出英文。
- **屏幕键盘**：xochitl 的屏幕键盘以 `QInputMethodEvent` 把单个字母“提交”给编辑器，模块在半路截下，同样交给 Rime。
- **上屏**：选中的词以 `QInputMethodEvent::setCommitString` 发给当前焦点对象，这和屏幕键盘自己输入文字走的是同一个入口，所以笔记编辑器按原生方式排版和显示。

整个过程不修改 xochitl 的任何代码或文件。

## 2. 拼音引擎：Rime 子进程

`pinyin-rime` 是一个很小的 C 程序，把 librime 包装成按行通信的协议：

```
key <keysym> <mask>   按键（X11 keysym 与 Rime 修饰键掩码）
pick <i>              选择当前页第 i 个候选
page <0|1>            上一页 / 下一页
ascii <0|1>           英文 / 中文
clear / commit        清空 / 上屏原始字母
```

每个请求回复一行 JSON：`{"handled","commit","preedit","cands","page","last","ascii"}`。

把引擎放在子进程里，是为了让 librime 及其依赖（Boost、LevelDB、OpenCC、Lua……）
与 xochitl 的进程隔离：库版本冲突或引擎崩溃都不会拖垮界面，模块会自动重启引擎。
雾凇拼音的方案与词库在构建时就已预编译好，平板上启动只需几百毫秒。

## 3. 候选栏：放进 xochitl 自己的界面

候选栏是一段 QML，用 xochitl 自己的 QML 引擎创建，挂进界面的元素树。有两个细节：

- **放对图层**：xochitl 的窗口下叠着好几个全屏图层（快捷键层、手势层、主界面层，屏幕键盘另在一层）。候选栏必须挂到**当前焦点所在的那一层**，否则会被上层完全盖住；横竖屏旋转也因此自动跟随。
- **不挡点击**：候选栏的根节点是零尺寸的锚点，内容在它之外绘制。如果用一个铺满全屏的透明层来承载，xochitl 判断“点在哪里”时会先碰到它，导致屏幕键盘等功能失灵。

### 找到光标

普通 Qt 输入框会通过 `ImCursorRectangle` 告诉输入法光标在哪里，但笔记本的文字是
xochitl 的场景（SceneView）自己绘制的，这个查询返回无效值。模块于是向上查找场景的
控制器，读取它公开的属性：

- `controller.textCursorPosition`：光标位置（页面坐标，位于文字基线）
- `controller.textLineHeight`：行高

再调用场景视图的 `sceneToView()` 换算成屏幕坐标。候选栏据此放在光标下一行、左端与
光标对齐；下方放不下（例如被屏幕键盘遮住，`keyboardMargin`）时改放到光标上方。

## 4. 为什么刷新快、没有残影

候选栏是 xochitl 界面的一部分，所以由 xochitl 自己的墨水屏显示管线负责刷新：只更新
变化的那一小块区域，并为文字区域选用快速的黑白刷新方式。候选栏只用纯黑和纯白
（首选项为黑底白字），正适合快速刷新而不留灰色残影。

## 5. 屏幕键盘的中文布局

xochitl 的屏幕键盘按设置里的语言加载 `:/misc/keyboards/<语言>/keyboard_layout.json`。
系统没有 `zh_CN` 布局，而曾经装过其他中文键盘插件的设备，设置往往停留在 `zh_CN`，
屏幕键盘就会弹出一片空白。模块启动时若发现缺少 `zh_CN`，会在内存里构建一棵 Qt
资源树，把系统自带的 `en_US` 布局同时以 `zh_CN` 的名字提供出来。键盘照常显示
QWERTY，打出的字母再由拼音引擎转换。整个过程不复制、不修改系统文件。

## 6. 稳定性设计

- **崩溃保护**：模块记录 xochitl 的启动次数（按开机 ID 区分）。同一次开机里连续 3 次启动都没撑过 1 分钟，模块就在本次开机内停用；xochitl 自身在 10 分钟内失败 4 次才会进入紧急模式，因此不会因为输入法陷入循环。下次开机会自动再试。
- **手动开关**：安装目录下存在 `disabled` 文件时，模块不做任何事。
- **系统更新**：reMarkable 采用 A/B 分区更新，新系统分区里没有我们添加的文件。`paperpro-pinyin-mirror.timer` 每 10 分钟检查一次 swupdate 的状态，更新完整写入后把配置复制过去。
