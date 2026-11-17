import QtQuick
import QtQuick.Controls

// Label on the left, toggle on the right (settings rows).
Switch {
    id: s
    property string description: ""
    padding: 6
    font.pixelSize: Theme.fontBody
    hoverEnabled: true

    indicator: Rectangle {
        implicitWidth: 40
        implicitHeight: 22
        x: s.width - width - s.rightPadding
        y: (s.height - height) / 2
        radius: 11
        color: s.checked ? (s.hovered ? Theme.accentHover : Theme.accent) : (s.hovered ? Theme.borderStrong : Theme.track)
        opacity: s.enabled ? 1 : 0.5
        Rectangle {
            width: 16
            height: 16
            radius: 8
            y: 3
            x: s.checked ? parent.width - width - 3 : 3
            color: "white"
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
        }
    }
    contentItem: Column {
        rightPadding: s.indicator.width + 16
        spacing: 2
        Text {
            width: parent.width - parent.rightPadding
            text: s.text
            font: s.font
            color: s.enabled ? Theme.text : Theme.textDisabled
            wrapMode: Text.WordWrap
        }
        Text {
            width: parent.width - parent.rightPadding
            visible: s.description !== ""
            text: s.description
            font.pixelSize: Theme.fontSmall
            color: Theme.textDim
            wrapMode: Text.WordWrap
        }
    }
}
