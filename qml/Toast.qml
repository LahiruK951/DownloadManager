import QtQuick
import QtQuick.Controls

// Transient message above the status bar.
Popup {
    id: toast
    property string text: ""

    function show(message) {
        if (!message)
            return
        text = message
        open()
        hideTimer.restart()
    }

    parent: Overlay.overlay
    x: (parent.width - width) / 2
    y: parent.height - height - 56
    padding: 0
    modal: false
    focus: false
    closePolicy: Popup.CloseOnPressOutside

    Timer { id: hideTimer; interval: 3500; onTriggered: toast.close() }

    background: Rectangle {
        radius: 10
        color: Theme.dark ? "#3a3f47" : "#1f2328"
    }
    contentItem: Row {
        leftPadding: 16
        rightPadding: 16
        topPadding: 10
        bottomPadding: 10
        spacing: 10
        Text {
            text: Theme.icons.info
            font.family: Theme.iconFont
            font.pixelSize: 13
            color: "#9cc0ff"
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: toast.text
            color: "white"
            font.pixelSize: Theme.fontBody
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 140 }
            NumberAnimation { property: "y"; from: toast.parent.height - toast.height - 40; to: toast.parent.height - toast.height - 56; duration: 160; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 160 } }
}
