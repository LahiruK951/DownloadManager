import QtQuick
import QtQuick.Controls

CheckBox {
    id: c
    spacing: 10
    padding: 4
    font.pixelSize: Theme.fontBody
    hoverEnabled: true

    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: c.leftPadding
        y: (c.height - height) / 2
        radius: 5
        color: c.checked ? (c.hovered ? Theme.accentHover : Theme.accent) : Theme.inputBg
        border.width: c.checked ? 0 : 1
        border.color: c.hovered ? Theme.borderStrong : Theme.border
        opacity: c.enabled ? 1 : 0.5
        Text {
            anchors.centerIn: parent
            visible: c.checked
            text: Theme.icons.check
            font.family: Theme.iconFont
            font.pixelSize: 11
            color: "white"
        }
    }
    contentItem: Text {
        leftPadding: c.indicator.width + c.spacing
        text: c.text
        font: c.font
        color: c.enabled ? Theme.text : Theme.textDisabled
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap
    }
}
