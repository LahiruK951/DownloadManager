import QtQuick
import QtQuick.Layouts

// Titled group of settings rows on a rounded card.
ColumnLayout {
    id: card
    property string title: ""
    property string description: ""
    default property alias rows: body.data

    spacing: 8

    Text {
        visible: card.title !== ""
        text: card.title
        color: Theme.text
        font.pixelSize: 14
        font.weight: Font.DemiBold
        Layout.leftMargin: 2
    }
    Text {
        visible: card.description !== ""
        Layout.fillWidth: true
        Layout.leftMargin: 2
        text: card.description
        color: Theme.textDim
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: body.implicitHeight + 8
        radius: Theme.radiusLarge
        color: Theme.surface
        border.color: Theme.border
        ColumnLayout {
            id: body
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 4
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            spacing: 0
        }
    }
}
