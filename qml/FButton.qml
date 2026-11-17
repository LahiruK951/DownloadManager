import QtQuick
import QtQuick.Controls

// kind: "primary" | "secondary" | "ghost" | "danger"
Button {
    id: b
    property string kind: "secondary"
    property string glyph: ""

    implicitHeight: Theme.controlHeight
    leftPadding: text === "" ? 9 : 16
    rightPadding: text === "" ? 9 : 16
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    font.pixelSize: Theme.fontBody
    font.weight: Font.Medium

    readonly property color fg: (kind === "primary" || kind === "danger") ? "white"
                              : kind === "ghost" ? Theme.accent : Theme.text

    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 7
            Text {
                visible: b.glyph !== ""
                text: b.glyph
                font.family: Theme.iconFont
                font.pixelSize: 12
                color: b.fg
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                visible: b.text !== ""
                text: b.text
                font: b.font
                color: b.fg
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    background: Rectangle {
        radius: Theme.radius
        opacity: b.enabled ? 1 : 0.45
        color: {
            if (b.kind === "primary")
                return b.down ? Theme.accentPressed : (b.hovered ? Theme.accentHover : Theme.accent)
            if (b.kind === "danger")
                return b.down ? Qt.darker(Theme.error, 1.2) : (b.hovered ? Qt.lighter(Theme.error, 1.1) : Theme.error)
            if (b.kind === "ghost")
                return b.down ? Theme.pressed : (b.hovered ? Theme.hover : "transparent")
            return b.down ? Theme.pressed : (b.hovered ? Theme.hover : Theme.surface)
        }
        border.width: b.kind === "secondary" ? 1 : 0
        border.color: b.hovered ? Theme.borderStrong : Theme.border
        Rectangle { // keyboard focus ring
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            opacity: 0.5
            visible: b.visualFocus
        }
    }
}
