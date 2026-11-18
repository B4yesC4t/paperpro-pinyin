import QtQuick
import xofm.libs.epaper // FAST-REGION
// PaperPro 拼音的屏幕键盘：盖在 xochitl 自己的键盘（vk）上，位置、大小、显示与否都跟着它。
// 按键经 ime 交给输入法，与 xochitl 键盘走同一条路（提交文字 / 删除 / 换行），
// 所以拼音、英文、设置页里的编辑框都照常工作。
Rectangle {
    id: kb
    property Item vk                 // xochitl 的键盘
    property bool chinese: true      // 输入法处于中文
    property bool latin: false       // 输入法让开的编辑框（API Key 等）：只打英文
    property bool composing: false   // 正在打拼音
    readonly property bool zh: chinese && !latin
    property int shift: 0            // 0 关，1 下一个字母大写，2 大写锁定
    property bool symbols: false     // 符号页
    property real lastShiftTap: 0

    x: vk ? vk.x : 0
    y: vk ? vk.y : 0
    width: vk ? vk.width : 0
    height: vk ? vk.height : 0
    visible: vk ? vk.visible && vk.height > 0 : false
    z: 100
    color: "white"
    onVisibleChanged: if (!visible) { shift = 0; symbols = false }

    component FastRegion: ScreenModeItem {} // FAST-REGION
    property int fastMode: 2   // EPScreenModeItem::Animation（直接写数字会被当成未知枚举）
    FastRegion { anchors.fill: parent; mode: kb.fastMode }
    // 键与键之间的空隙也接住触摸，免得落到下面 xochitl 的键盘上。
    MouseArea { anchors.fill: parent }
    Rectangle { width: parent.width; height: 2; color: "black" }

    // ---- 外观 ----------------------------------------------------------------------
    // 参照 iPad Pro 13 寸与 Gboard 平板键盘：每行 14 列、左右都排满，字母行像实体键盘那样错开；
    // 数字行矮一些。墨水屏上只用黑、白和一种浅灰：字母键白底黑框，功能键浅灰，回车实心黑。
    readonly property int cols: 14
    readonly property real padX: 8
    readonly property real padY: 8
    readonly property real gap: 10
    readonly property real unit: (width - 2 * padX + gap) / cols
    readonly property real numberRowScale: 0.78
    readonly property real rowUnit: (height - 2 * padY + gap) / (4 + numberRowScale)
    readonly property string iconFont: "Material Icons Outlined"
    readonly property string textFont: "Noto Sans"   // 无衬线；汉字回退到 Noto Sans CJK
    readonly property color grey: "#d8d8d8"

    // ---- 布局 ----------------------------------------------------------------------
    // 每个键：c 打出的字，w 宽度（列数），k 种类，s 长按（或 Shift）打出的字
    readonly property var digitsZh: ["！", "@", "#", "¥", "%", "……", "&", "*", "（", "）"]
    readonly property var digitsEn: ["!", "@", "#", "$", "%", "^", "&", "*", "(", ")"]
    function numberRow() {
        var r = [{ c: zh ? "～" : "~", k: "raw" }], s = zh ? digitsZh : digitsEn
        for (var i = 0; i < 10; i++) r.push({ c: String((i + 1) % 10), s: s[i], k: "digit" })
        return r.concat([{ c: zh ? "——" : "-", k: "raw" }, { c: zh ? "（" : "(", k: "raw" }, { c: zh ? "）" : ")", k: "raw" }])
    }
    function letters(str) { return str.split("").map(function (ch) { return { c: ch, k: "letter" } }) }
    function punct(zhs, ens) { return { c: zh ? zhs : ens, k: "raw" } }
    readonly property var rows: symbols ? symbolRows() : letterRows()
    function letterRows() {
        return [
            numberRow(),
            [{ k: "tab", w: 1.5 }].concat(letters("qwertyuiop"), [{ k: "bs", w: 2.5 }]),
            [{ k: "lang", w: 1.75 }].concat(letters("asdfghjkl"), [punct("：", ":"), { k: "enter", w: 2.25 }]),
            [{ k: "shift", w: 2.25 }].concat(letters("zxcvbnm"), [punct("，", ","), punct("。", "."), punct("？", "?"),
                                                                  { k: "shift", w: 1.75 }]),
            bottomRow()
        ]
    }
    function chars(str) { return str.split(" ").map(function (ch) { return { c: ch, k: "raw" } }) }
    function symbolRows() {
        var r = zh ? [chars("， 。 、 ？ ！ ； ： “ ” ‘ ’"), chars("（ ） 《 》 【 】 「 」 『 』"),
                      chars("@ # % & * + = / \\ _ - …… ～ ·")]
                   : [chars(", . ? ! ' \" : ; [ ] {"), chars("} < > | ` ^ $ € £ ¥"),
                      chars("@ # % & * + = / \\ _ - ~ … ·")]
        return [
            numberRow(),
            [{ k: "tab", w: 1.5 }].concat(r[0], [{ k: "bs", w: 1.5 }]),
            [{ k: "lang", w: 1.75 }].concat(r[1], [{ k: "enter", w: 2.25 }]),
            r[2],
            bottomRow()
        ]
    }
    function bottomRow() {
        return [{ k: "sym", w: 1.75 }, { k: "settings", w: 1.25 }, { k: "mic", w: 1.25 }, { k: "space", w: 5.75 },
                { k: "left" }, { k: "right" }, { k: "hide", w: 2 }]
    }

    // ---- 按键动作 --------------------------------------------------------------------
    function label(s) {
        switch (s.k) {
        case "bs": return "\ue14a"
        case "enter": return "\ue31b"
        case "shift": return shift === 2 ? "\ue318" : "\ue5d8"
        case "settings": return "\ue8b8"
        case "mic": return "\ue029"
        case "hide": return "\ue31a"
        case "tab": return "\ue31c"
        case "left": return "\ue5cb"
        case "right": return "\ue5cc"
        case "sym": return symbols ? (zh ? "拼音" : "ABC") : (zh ? "符号" : "123")
        case "space": return zh ? "空格" : "space"
        case "lang": return ""
        case "letter": return upper() ? s.c.toUpperCase() : s.c
        case "digit": return shift > 0 ? s.s : s.c
        }
        return s.c
    }
    function isIcon(s) { return ["bs", "enter", "shift", "settings", "mic", "hide", "tab", "left", "right"].indexOf(s.k) >= 0 }
    function isFunction(s) { return ["letter", "digit", "raw", "space", "enter"].indexOf(s.k) < 0 }
    // 中文时字母键是小写（拼音）；按了 Shift 才是大写字母，直接上屏。
    function upper() { return shift > 0 }

    function tap(s) {
        switch (s.k) {
        case "bs": ime.oskBackspace(); return
        case "enter": ime.oskEnter(); return
        case "tab": ime.oskKey(Qt.Key_Tab); return
        case "left": ime.oskKey(Qt.Key_Left); return
        case "right": ime.oskKey(Qt.Key_Right); return
        case "shift":
            var now = Date.now()
            if (shift === 0) shift = 1
            else if (shift === 1 && now - lastShiftTap < 400) shift = 2
            else shift = 0
            lastShiftTap = now
            return
        case "sym": symbols = !symbols; shift = 0; return
        case "settings": ime.openSettings(); return
        case "mic": ime.voice(); return
        case "hide": ime.hideKeyboard(); return
        case "lang": ime.toggle(); return
        case "space": ime.oskType(" "); return
        case "letter":
            if (upper()) {
                ime.oskRaw(s.c.toUpperCase())
                if (shift === 1) shift = 0
            } else ime.oskType(s.c)
            return
        case "digit":
            if (shift > 0) {
                ime.oskRaw(s.s)
                if (shift === 1) shift = 0
            } else ime.oskType(s.c)      // 打拼音时数字选字
            return
        }
        ime.oskRaw(s.c)
    }
    function longPress(s) {
        if (s.k === "space") { ime.oskVoice(true); return true }
        if (s.k === "bs" || s.k === "left" || s.k === "right") { repeat.key = s.k; repeat.start(); tap(s); return true }
        if (s.s !== undefined) { ime.oskRaw(s.s); return true }
        return false
    }
    function release(s) {
        if (s.k === "space") ime.oskVoice(false)
        repeat.stop()
    }
    Timer { id: repeat; property string key; interval: 90; repeat: true; onTriggered: kb.tap({ k: key }) }

    // ---- 绘制 ------------------------------------------------------------------------
    component Key: Rectangle {
        id: key
        required property var modelData
        required property real rowHeight
        readonly property var s: modelData
        readonly property bool down: th.pressed
        readonly property bool dark: s.k === "enter" ? !down : (down || (s.k === "shift" && kb.shift === 2))
        property bool held: false
        width: (s.w || 1) * kb.unit - kb.gap
        height: rowHeight - kb.gap
        radius: 12
        border.width: s.k === "shift" && kb.shift === 1 ? 5 : 2
        border.color: "black"
        color: dark ? "black" : (kb.isFunction(s) ? kb.grey : "white")
        Text {
            anchors.centerIn: parent
            visible: s.k !== "lang"
            text: kb.label(s)
            color: key.dark ? "white" : "black"
            font.family: kb.isIcon(s) ? kb.iconFont : kb.textFont
            font.pixelSize: kb.isIcon(s) ? 42
                          : (s.k === "letter" ? 46 : (s.k === "digit" ? 40 : (s.k === "sym" || s.k === "space" ? 28 : 40)))
        }
        Text {   // 长按（或 Shift）打出的符号
            visible: s.k === "digit" && kb.shift === 0
            anchors.right: parent.right; anchors.top: parent.top
            anchors.rightMargin: 10; anchors.topMargin: 4
            text: s.s || ""
            color: key.dark ? "white" : "black"
            font.family: kb.textFont
            font.pixelSize: 20
        }
        Row {    // 中/英：当前的那个字黑、粗
            visible: s.k === "lang"
            anchors.centerIn: parent
            spacing: 2
            Text { text: "中"; font.pixelSize: 34; font.bold: kb.zh; color: key.dark ? "white" : (kb.zh ? "black" : "#777777") }
            Text { text: "/"; font.pixelSize: 30; color: key.dark ? "white" : "#777777" }
            Text { text: "英"; font.pixelSize: 34; font.bold: !kb.zh; color: key.dark ? "white" : (!kb.zh ? "black" : "#777777") }
        }
        TapHandler {
            id: th
            longPressThreshold: 0.4
            onTapped: kb.tap(key.s)
            onLongPressed: key.held = kb.longPress(key.s)
            onPressedChanged: if (!pressed) { if (key.held) kb.release(key.s); key.held = false }
        }
    }

    Column {
        x: kb.padX; y: kb.padY
        spacing: 0
        Repeater {
            model: kb.rows
            delegate: Row {
                required property var modelData
                required property int index
                readonly property real h: kb.rowUnit * (index === 0 ? kb.numberRowScale : 1)
                height: h
                spacing: kb.gap
                Repeater { model: modelData; delegate: Key { rowHeight: h } }
            }
        }
    }
}
