import QtQuick
import QtQuick.Controls

// Pill-shaped segmented control.
Rectangle {
    id: seg
    property var model: []
    property int currentIndex: 0
    property var tips: []
    property int itemHeight: 28
    signal activated(int index)

    implicitWidth: row.implicitWidth + 6
    implicitHeight: itemHeight + 6
    radius: height / 2
    color: Theme.surfaceAlt
    border.color: Theme.border

    Row {
        id: row
        anchors.centerIn: parent
        Repeater {
            model: seg.model
            delegate: AbstractButton {
                id: btn
                required property string modelData
                required property int index
                readonly property bool current: seg.currentIndex === index
                implicitWidth: label.implicitWidth + 26
                implicitHeight: seg.itemHeight
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                onClicked: seg.activated(index)
                background: Rectangle {
                    radius: height / 2
                    color: btn.current ? Theme.accent : (btn.hovered ? Theme.hover : "transparent")
                    Behavior on color { ColorAnimation { duration: 120 } }
                }
                contentItem: Text {
                    id: label
                    text: btn.modelData
                    color: btn.current ? "white" : Theme.text
                    font.pixelSize: Theme.fontSmall + 0.5
                    font.weight: btn.current ? Font.DemiBold : Font.Normal
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                ToolTip.visible: hovered && seg.tips.length > index && seg.tips[index] !== ""
                ToolTip.text: seg.tips.length > index ? seg.tips[index] : ""
                ToolTip.delay: 400
            }
        }
    }
}
