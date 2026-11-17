import QtQuick
import QtQuick.Controls

AbstractButton {
    id: chip
    property string label
    property int count: 0
    property bool active: false

    implicitHeight: 28
    implicitWidth: row.implicitWidth + 24
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: 14
        color: chip.active ? Theme.accent : (chip.hovered ? Theme.hover : "transparent")
        border.width: chip.active ? 0 : 1
        border.color: Theme.border
    }
    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 6
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
