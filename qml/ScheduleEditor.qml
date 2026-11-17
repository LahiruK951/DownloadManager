import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Weekly window editor: day toggles + from/to time. days bit0 = Monday.
RowLayout {
    id: ed
    property int days: 0x7F
    property int fromMin: 60
    property int toMin: 420
    signal edited()

    spacing: 10

    function minToText(m) { return ("0" + Math.floor(m / 60)).slice(-2) + ":" + ("0" + (m % 60)).slice(-2) }
    function textToMin(t) {
        const p = t.split(":")
        const h = parseInt(p[0]), m = parseInt(p[1])
        return (isNaN(h) || isNaN(m)) ? -1 : Math.min(23, h) * 60 + Math.min(59, m)
    }

    Row {
        spacing: 3
        Repeater {
            model: [qsTr("M"), qsTr("T"), qsTr("W"), qsTr("T"), qsTr("F"), qsTr("S"), qsTr("S")]
            delegate: AbstractButton {
                id: dayBtn
                required property string modelData
                required property int index
                readonly property bool on: (ed.days & (1 << index)) !== 0
                implicitWidth: 26
                implicitHeight: 26
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                onClicked: { ed.days = ed.days ^ (1 << index); ed.edited() }
                background: Rectangle {
                    radius: 13
                    color: dayBtn.on ? Theme.accent : (dayBtn.hovered ? Theme.hover : Theme.surfaceAlt)
                    border.color: Theme.border
                    border.width: dayBtn.on ? 0 : 1
                }
                contentItem: Text {
                    text: dayBtn.modelData
                    color: dayBtn.on ? "white" : Theme.textDim
                    font.pixelSize: 11
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: [qsTr("Monday"), qsTr("Tuesday"), qsTr("Wednesday"), qsTr("Thursday"), qsTr("Friday"), qsTr("Saturday"), qsTr("Sunday")][index]
            }
        }
    }

    Label { text: qsTr("from"); color: Theme.textDim }
    TextField {
        id: fromField
        implicitWidth: 64
        horizontalAlignment: Text.AlignHCenter
        inputMask: "99:99"
        color: Theme.text
        text: ed.minToText(ed.fromMin)
        onEditingFinished: { const m = ed.textToMin(text); if (m >= 0) { ed.fromMin = m; ed.edited() } text = ed.minToText(ed.fromMin) }
    }
    Label { text: qsTr("to"); color: Theme.textDim }
    TextField {
        id: toField
        implicitWidth: 64
        horizontalAlignment: Text.AlignHCenter
        inputMask: "99:99"
        color: Theme.text
        text: ed.minToText(ed.toMin)
        onEditingFinished: { const m = ed.textToMin(text); if (m >= 0) { ed.toMin = m; ed.edited() } text = ed.minToText(ed.toMin) }
    }
}
