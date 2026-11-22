import QtQuick
import QtQuick.Controls

TextField {
    Accessible.name: placeholderText
    id: f
    implicitHeight: 36
    leftPadding: 12
    rightPadding: 12
    font.pixelSize: Theme.fontBody
    color: Theme.text
    placeholderTextColor: Theme.textDisabled
    selectionColor: Theme.accent
    selectedTextColor: "white"
    selectByMouse: true
    verticalAlignment: TextInput.AlignVCenter

    background: Rectangle {
        radius: Theme.radius
        color: Theme.inputBg
        opacity: f.enabled ? 1 : 0.6
        border.width: f.activeFocus ? 2 : 1
        border.color: f.activeFocus ? Theme.accent : (f.hovered ? Theme.borderStrong : Theme.border)
    }
}
