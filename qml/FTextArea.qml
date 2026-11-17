import QtQuick
import QtQuick.Controls

// Multi-line input with the same frame as FTextField.
Rectangle {
    id: frame
    property alias text: area.text
    property alias placeholderText: area.placeholderText
    property alias area: area

    implicitHeight: 92
    radius: Theme.radius
    color: Theme.inputBg
    border.width: area.activeFocus ? 2 : 1
    border.color: area.activeFocus ? Theme.accent : (hover.hovered ? Theme.borderStrong : Theme.border)

    HoverHandler { id: hover }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 2
        clip: true
        TextArea {
            id: area
            wrapMode: TextArea.WrapAnywhere
            padding: 10
            font.pixelSize: Theme.fontBody
            color: Theme.text
            placeholderTextColor: Theme.textDisabled
            selectionColor: Theme.accent
            selectedTextColor: "white"
            selectByMouse: true
            background: null
        }
    }
}
