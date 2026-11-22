import QtQuick
import QtQuick.Controls

AbstractButton {
    id: chip
    property string label
    property int count: 0
    property bool active: false
    property color dot: "transparent" // user tag colour
    signal rightClicked()

    implicitHeight: 28
    implicitWidth: row.implicitWidth + 24
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    Accessible.role: Accessible.Button
    Accessible.name: count > 0 ? label + ", " + count : label
    Accessible.checkable: true
    Accessible.checked: active

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: chip.rightClicked()
    }

    background: Rectangle {
        radius: 14
        color: chip.active ? Theme.accent : (chip.hovered ? Theme.hover : "transparent")
        border.width: chip.active ? 0 : 1
        border.color: Theme.border
        Rectangle { // keyboard focus ring
            anchors.fill: parent
            anchors.margins: -2
            radius: parent.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.accent
            opacity: 0.6
            visible: chip.visualFocus
        }
    }
    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 6
            Rectangle {
                visible: chip.dot.a > 0
                width: 8; height: 8; radius: 4
                anchors.verticalCenter: parent.verticalCenter
                color: chip.dot
                border.width: chip.active ? 1 : 0
                border.color: "white"
            }
            Text {
                text: chip.label
                color: chip.active ? "white" : Theme.text
                font.pixelSize: 13
            }
            Text {
                text: chip.count
                visible: chip.count > 0
                color: chip.active ? "#dbe7ff" : Theme.textDim
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
}
