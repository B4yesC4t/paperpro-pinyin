import QtQuick
// A zero-size anchor: a full-size item over xochitl's UI would be what its
// own hit tests (childAt) find. Children are not clipped, and are tappable.
Item {
    id: root
    x: 0; y: 0; z: 1000000
    width: 0; height: 0
    readonly property real areaW: parent ? parent.width : 0
    readonly property real areaH: parent ? parent.height : 0
    property var cands: []
    property string preedit: ""
    property bool chinese: true
    property bool editing: false
    property bool hasPrev: false
    property bool hasNext: false
    property rect cursor: Qt.rect(0, 0, 0, 0)
    property real bottomMargin: 0   // the on-screen keyboard, when it is up
    property bool settingsOpen: false
    onSettingsOpenChanged: { bt.watch(settingsOpen); ime.settingsShown(settingsOpen) }
    readonly property bool cursorOk: cursor.height > 0 && cursor.y >= 0 && cursor.y + cursor.height <= areaH
                                     && cursor.x >= 0 && cursor.x <= areaW
    // As on a computer: on the line below the text cursor, starting where it
    // is; above the cursor when there is no room below.
    function placeY(h) {
        if (!cursorOk) return 24
        var below = cursor.y + cursor.height + 10
        if (below + h <= areaH - bottomMargin - 8) return below
        return Math.max(8, cursor.y - h - 10)
    }
    function placeX(w) {
        if (!cursorOk) return (areaW - w) / 2
        return Math.max(8, Math.min(areaW - w - 8, cursor.x - 14))
    }

    Rectangle {
        id: bar
        visible: root.preedit.length > 0
        color: "white"; border.color: "black"; border.width: 2; radius: 8
        width: Math.min(root.areaW - 16, content.implicitWidth + 24)
        height: content.implicitHeight + 16
        x: root.placeX(width)
        y: root.placeY(height)
        Column {
            id: content
            x: 12; y: 8
            spacing: 2
            Text { text: root.preedit; font.pixelSize: 24; color: "black"; leftPadding: 4 }
            Row {
                spacing: 4
                Repeater {
                    model: root.cands
                    delegate: Rectangle {
                        width: cell.implicitWidth + 16; height: 54; radius: 6
                        color: index === 0 ? "black" : "white"
                        Row {
                            id: cell
                            anchors.centerIn: parent
                            spacing: 4
                            Text {
                                anchors.baseline: word.baseline
                                text: index + 1; font.pixelSize: 22
                                color: index === 0 ? "white" : "black"
                            }
                            Text {
                                id: word
                                text: modelData; font.pixelSize: 38
                                color: index === 0 ? "white" : "black"
                            }
                        }
                        TapHandler { onTapped: ime.pick(index) }
                    }
                }
                Rectangle {
                    visible: root.hasPrev || root.hasNext
                    width: 44; height: 54; color: "white"
                    Text { anchors.centerIn: parent; text: "‹"; font.pixelSize: 38; color: root.hasPrev ? "black" : "#bbbbbb" }
                    TapHandler { onTapped: ime.page(-1) }
                }
                Rectangle {
                    visible: root.hasPrev || root.hasNext
                    width: 44; height: 54; color: "white"
                    Text { anchors.centerIn: parent; text: "›"; font.pixelSize: 38; color: root.hasNext ? "black" : "#bbbbbb" }
                    TapHandler { onTapped: ime.page(1) }
                }
            }
        }
    }

    // 中/英, shown briefly by the cursor when it changes or typing starts.
    property bool modeShown: false
    function flash() { if (editing) { modeShown = true; modeTimer.restart() } else modeShown = false }
    onChineseChanged: flash()
    onEditingChanged: flash()
    Timer { id: modeTimer; interval: 1500; onTriggered: root.modeShown = false }
    Rectangle {
        visible: root.modeShown && !bar.visible
        width: 56; height: 46; radius: 6
        color: root.chinese ? "black" : "white"; border.color: "black"; border.width: 2
        x: root.placeX(width)
        y: root.placeY(height)
        Text {
            anchors.centerIn: parent
            text: root.chinese ? "中" : "英"
            font.pixelSize: 28; color: root.chinese ? "white" : "black"
        }
        TapHandler { onTapped: ime.toggle() }
    }

    // ---- 编辑时的工具条：中/英、语音、设置 -------------------------------------------
    // 停在右下角（屏幕键盘弹出时在键盘上方），手指够得着，也不挡住正在写的那一行。
    component ToolButton: Rectangle {
        id: tb
        property string label
        property bool dark: false
        signal clicked
        width: Math.max(96, tbText.implicitWidth + 36); height: 72
        color: dark ? "black" : "white"
        border.color: "black"; border.width: 2
        Text {
            id: tbText
            anchors.centerIn: parent
            text: tb.label; font.pixelSize: 32
            color: tb.dark ? "white" : "black"
        }
        // MouseArea（而不是 TapHandler）：接住这次点击，不让它落到下面的笔记里。
        MouseArea { anchors.fill: parent; onClicked: tb.clicked() }
    }
    Row {
        id: tools
        visible: root.editing && !root.settingsOpen
        x: root.areaW - width - 24
        y: root.areaH - root.bottomMargin - height - 24
        spacing: -2
        ToolButton { label: root.chinese ? "中" : "英"; dark: root.chinese; onClicked: ime.toggle() }
        ToolButton { label: "语音"; onClicked: ime.voice() }
        ToolButton { label: "设置"; onClicked: root.settingsOpen = true }
    }

    // ---- 设置页 ---------------------------------------------------------------------
    component Button: Rectangle {
        id: btn
        property string label
        property bool primary: false
        signal clicked
        width: btnText.implicitWidth + 64; height: 88; radius: 10
        color: primary && enabled ? "black" : "white"
        border.color: enabled ? "black" : "#aaaaaa"; border.width: 3
        Text {
            id: btnText
            anchors.centerIn: parent
            text: btn.label; font.pixelSize: 36
            color: btn.primary && btn.enabled ? "white" : (btn.enabled ? "black" : "#aaaaaa")
        }
        MouseArea { anchors.fill: parent; onClicked: btn.clicked() }
    }
    component Heading: Column {
        property string text
        width: parent ? parent.width : 0
        spacing: 12
        topPadding: 36
        Text { text: parent.text; font.pixelSize: 44; font.bold: true; color: "black" }
        Rectangle { width: parent.width; height: 3; color: "black" }
    }
    component Note: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.Wrap
        font.pixelSize: 30; color: "#444444"; lineHeight: 1.2
    }

    Rectangle {
        id: page
        visible: root.settingsOpen
        x: 0; y: 0
        width: root.areaW; height: root.areaH
        color: "white"
        // 接住所有点击：设置页打开时，下面的笔记不受影响。
        MouseArea { anchors.fill: parent }

        Flickable {
            anchors.fill: parent
            contentHeight: settingsCol.height + 120
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: settingsCol
                x: Math.max(48, (page.width - 1300) / 2)
                width: Math.min(1300, page.width - 96)
                spacing: 28
                topPadding: 64

                Item {
                    width: parent.width; height: 100
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "输入法设置"; font.pixelSize: 64; font.bold: true; color: "black"
                    }
                    Button {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        label: "完成"; primary: true
                        onClicked: root.settingsOpen = false
                    }
                }

                // 蓝牙麦克风
                Heading { text: "蓝牙麦克风" }
                Item {
                    width: parent.width; height: 96
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Text { text: "蓝牙"; font.pixelSize: 40; color: "black" }
                        Text {
                            text: bt.state === "on" ? "已打开"
                                : bt.state === "starting" ? "正在打开…"
                                : bt.state === "error" ? "出错了" : "已关闭"
                            font.pixelSize: 28; color: "#444444"
                        }
                    }
                    Button {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        label: bt.state === "on" || bt.state === "starting" ? "关闭蓝牙" : "打开蓝牙"
                        primary: !(bt.state === "on" || bt.state === "starting")
                        enabled: bt.state !== "starting"
                        onClicked: bt.setEnabled(!(bt.state === "on"))
                    }
                }
                Note { visible: bt.message.length > 0; text: bt.message; color: "black" }
                Note {
                    visible: bt.state === "on"
                    text: "把麦克风或耳机切到配对模式（通常是长按电源键或配对键，直到指示灯快速闪烁），再点“搜索设备”。"
                }
                Button {
                    visible: bt.state === "on"
                    label: bt.scanning ? "停止搜索" : "搜索设备"
                    onClicked: bt.scan(!bt.scanning)
                }
                Repeater {
                    model: bt.state === "on" ? bt.devices : []
                    delegate: Item {
                        width: settingsCol.width; height: 128
                        readonly property bool isMic: modelData.address === bt.mic
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - actions.width - 24
                            spacing: 6
                            Text {
                                width: parent.width; elide: Text.ElideRight
                                text: modelData.name; font.pixelSize: 38; color: "black"
                            }
                            Text {
                                text: (isMic ? "语音输入使用这个麦克风" : modelData.paired ? "已配对" : "未配对")
                                      + (modelData.audio ? "" : " · 不像是音频设备")
                                font.pixelSize: 26; color: "#555555"
                            }
                        }
                        Row {
                            id: actions
                            anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                            spacing: 16
                            Button {
                                visible: !modelData.paired
                                label: bt.busy === modelData.path ? "配对中…" : "配对"
                                primary: modelData.audio
                                enabled: bt.busy.length === 0
                                onClicked: bt.pair(modelData.path)
                            }
                            Button {
                                visible: modelData.paired && !isMic
                                label: "用作麦克风"
                                onClicked: bt.useAsMic(modelData.address)
                            }
                            Button {
                                visible: modelData.paired
                                label: "忘记"
                                onClicked: bt.forget(modelData.path)
                            }
                        }
                        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#999999" }
                    }
                }

                // 语音识别
                Heading { text: "语音识别（DeepSeek）" }
                Item {
                    width: parent.width; height: 96
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Text { text: "API Key"; font.pixelSize: 40; color: "black" }
                        Text {
                            text: voice.keyHint.length > 0 ? "已设置（" + voice.keyHint + "）" : "未设置"
                            font.pixelSize: 28; color: "#444444"
                        }
                    }
                    Button {
                        anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                        label: voice.testState === "testing" ? "测试中…" : "测试连接"
                        enabled: voice.testState !== "testing"
                        onClicked: voice.testKey()
                    }
                }
                Note { visible: voice.testMessage.length > 0; text: voice.testMessage; color: "black" }

                // 切换方式
                Heading { text: "怎样切换" }
                Note {
                    color: "black"
                    text: "打字：拼音与英文之间，单击 Shift、按 Ctrl+空格，或点右下角工具条的「中/英」。\n"
                        + "说话：点工具条的「语音」开始，说完再点一下，文字会出现在光标处；外接键盘上单击 Alt 也一样，按住 Alt 说话、松开即结束。Esc 取消。\n"
                        + "两者互不影响：语音输入完，继续打字时仍是原来的中文或英文。"
                }
            }
        }
    }
}
