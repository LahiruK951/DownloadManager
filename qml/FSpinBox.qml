import QtQuick
import QtQuick.Controls

SpinBox {
    id: sp
    property string zeroText: ""   // e.g. "Unlimited" shown instead of 0
    textFromValue: (v) => (v === 0 && zeroText !== "") ? zeroText : String(v)
    valueFromText: (t) => (t === zeroText && zeroText !== "") ? 0 : (parseInt(t) || 0)
    implicitWidth: 128
    implicitHeight: Theme.controlHeight
    editable: true
    font.pixelSize: Theme.fontBody
    hoverEnabled: true

    contentItem: TextInput {
        text: sp.displayText
        font: sp.font
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: "white"
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !sp.editable
        validator: sp.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        selectByMouse: true
        clip: true
    }

    up.indicator: Rectangle {
        x: sp.width - width - 3
        y: 3
        implicitWidth: 28
        height: sp.height - 6
        radius: 6
        color: sp.up.pressed ? Theme.pressed : (sp.up.hovered ? Theme.hover : "transparent")
        Text {
            anchors.centerIn: parent
            text: Theme.icons.add
            font.family: Theme.iconFont
            font.pixelSize: 10
            color: sp.value < sp.to ? Theme.text : Theme.textDisabled
        }
    }
    down.indicator: Rectangle {
        x: 3
        y: 3
        implicitWidth: 28
        height: sp.height - 6
        radius: 6
        color: sp.down.pressed ? Theme.pressed : (sp.down.hovered ? Theme.hover : "transparent")
        Text {
            anchors.centerIn: parent
            text: String.fromCharCode(0xE738) // Remove (minus)
            font.family: Theme.iconFont
            font.pixelSize: 10
            color: sp.value > sp.from ? Theme.text : Theme.textDisabled
        }
    }
    background: Rectangle {
        radius: Theme.radius
        color: Theme.inputBg
        border.width: sp.activeFocus ? 2 : 1
        border.color: sp.activeFocus ? Theme.accent : (sp.hovered ? Theme.borderStrong : Theme.border)
    }
}
