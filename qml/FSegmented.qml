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
            id: rep
            model: seg.model
            delegate: AbstractButton {
                id: btn
                required property string modelData
                required property int index
                readonly property bool current: seg.currentIndex === index
                implicitWidth: label.implicitWidth + 26
                implicitHeight: seg.itemHeight
                hoverEnabled: true
                focusPolicy: btn.current ? Qt.TabFocus : Qt.NoFocus // one Tab stop; arrows move inside
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checkable: true
                Accessible.checked: current
                onClicked: seg.activated(index)
                Keys.onLeftPressed: if (index > 0) { seg.activated(index - 1); rep.itemAt(index - 1).forceActiveFocus(Qt.TabFocusReason) }
                Keys.onRightPressed: if (index < seg.model.length - 1) { seg.activated(index + 1); rep.itemAt(index + 1).forceActiveFocus(Qt.TabFocusReason) }
                background: Rectangle {
                    radius: height / 2
                    color: btn.current ? Theme.accent : (btn.hovered ? Theme.hover : "transparent")
                    Behavior on color { ColorAnimation { duration: 120 } }
                    Rectangle { // keyboard focus ring
                        anchors.fill: parent
                        anchors.margins: -2
                        radius: parent.radius + 2
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.accent
                        opacity: 0.6
                        visible: btn.visualFocus
                    }
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
