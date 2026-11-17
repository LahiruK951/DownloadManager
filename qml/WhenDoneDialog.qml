import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Countdown before the "when all downloads are done" action; cancellable.
FDialog {
    id: dlg
    property int action: 0
    property int secondsLeft: 30
    title: qsTr("All downloads are complete")
    width: Math.min(440, parent.width - 48)
    closePolicy: Popup.NoAutoClose
    showClose: false

    readonly property string actionName: [qsTr("Do nothing"), qsTr("Sleep"), qsTr("Hibernate"), qsTr("Shut down"), qsTr("Exit FluxDM")][action]

    function start(a) {
        action = a
        secondsLeft = 30
        open()
        ticker.start()
    }

    Timer {
        id: ticker
        interval: 1000
        repeat: true
        onTriggered: {
            if (--dlg.secondsLeft <= 0) {
                stop()
                dlg.close()
                App.performWhenDone(dlg.action)
            }
        }
    }

    contentItem: RowLayout {
        spacing: 16
        Item {
            implicitWidth: 56
            implicitHeight: 56
            Rectangle { anchors.fill: parent; radius: 28; color: Theme.accentSoft }
            Text {
                anchors.centerIn: parent
                text: dlg.secondsLeft
                color: Theme.accent
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.text
            font.pixelSize: Theme.fontBody
            text: qsTr("Your computer will %1 in %2 seconds.").arg(dlg.actionName.toLowerCase()).arg(dlg.secondsLeft)
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: { ticker.stop(); App.whenDone = 0; dlg.close() } },
        FButton { kind: "primary"; text: dlg.actionName + " " + qsTr("now"); onClicked: { ticker.stop(); dlg.close(); App.performWhenDone(dlg.action) } }
    ]
}
