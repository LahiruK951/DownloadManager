import QtQuick
import QtQuick.Controls

Menu {
    id: m
    padding: 5
    implicitWidth: 260
    delegate: FMenuItem {}   // used for sub-menus declared inside this menu

    background: Rectangle {
        implicitWidth: 260
        radius: 10
        color: Theme.surface
        border.color: Theme.border
    }

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 70 } }
}
