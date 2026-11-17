import QtQuick
import QtQuick.Controls

// Bare toggle switch (no label) for SettingRow.
Switch {
    id: s
    padding: 0
    hoverEnabled: true
    implicitWidth: 40
    implicitHeight: 22
    indicator: Rectangle {
        implicitWidth: 40
        implicitHeight: 22
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
    contentItem: Item {}
}
