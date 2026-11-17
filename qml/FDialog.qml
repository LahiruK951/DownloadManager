import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Base dialog: title row with close button, rounded card, dimmed backdrop,
// footer row for buttons (put FButtons into `buttons`).
Dialog {
    id: d
    property string subtitle: ""
    property alias buttons: buttonRow.data
    property bool showClose: true

    modal: true
    anchors.centerIn: parent
    padding: 24
    topPadding: 4
    bottomPadding: 8

    Overlay.modal: Rectangle { color: Theme.overlay }

    background: Rectangle {
        radius: Theme.radiusLarge
        color: Theme.surface
        border.color: Theme.border
    }

    header: Item {
        implicitHeight: d.subtitle === "" ? 62 : 80
        Column {
            anchors.left: parent.left
            anchors.right: closeBtn.left
            anchors.leftMargin: 24
            anchors.rightMargin: 8
            anchors.top: parent.top
            anchors.topMargin: 22
            spacing: 4
            Text {
                width: parent.width
                text: d.title
                color: Theme.text
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: d.subtitle !== ""
                text: d.subtitle
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideMiddle
            }
        }
        IconButton {
            id: closeBtn
            visible: d.showClose
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.top: parent.top
            anchors.topMargin: 14
            glyph: Theme.icons.close
            glyphSize: 11
            size: 30
            onClicked: d.reject()
        }
    }

    footer: Item {
        implicitHeight: buttonRow.children.length > 0 ? 72 : 16
        RowLayout {
            id: buttonRow
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
        }
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 }
            NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 }
    }
}
