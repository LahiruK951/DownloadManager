import QtQuick
import QtQuick.Controls

AbstractButton {
    id: b
    property string glyph
    property string tip
    property color glyphColor: Theme.text
    property int size: 32
    property int glyphSize: 16

    implicitWidth: size
    implicitHeight: size
    hoverEnabled: true
    padding: 0
    focusPolicy: Qt.TabFocus // Tab reaches it; a click doesn't take focus from the list
    Accessible.role: Accessible.Button
    Accessible.name: tip

    background: Rectangle {
        radius: 6
        color: b.down ? Theme.pressed : (b.hovered && b.enabled ? Theme.hover : "transparent")
        Rectangle { // keyboard focus ring
            anchors.fill: parent
            anchors.margins: -2
            radius: parent.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            opacity: 0.6
            visible: b.visualFocus
        }
    }
    contentItem: Text {
        text: b.glyph
        font.family: Theme.iconFont
        font.pixelSize: b.glyphSize
        color: b.enabled ? b.glyphColor : Theme.textDisabled
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 600
}
