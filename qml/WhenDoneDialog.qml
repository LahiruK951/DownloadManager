import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Countdown before the "when all downloads are done" action; cancellable.
Dialog {
    id: dlg
    property int action: 0
    property int secondsLeft: 30
    modal: true
    anchors.centerIn: parent
    width: Math.min(440, parent.width - 40)
    padding: 20
    closePolicy: Popup.NoAutoClose

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

    background: Rectangle { color: Theme.surface; radius: 10; border.color: Theme.border }
    header: Label { text: qsTr("All downloads are complete"); color: Theme.text; font.pixelSize: 17; padding: 20; bottomPadding: 0 }

    contentItem: Text {
        wrapMode: Text.WordWrap
        color: Theme.text
        text: qsTr("%1 in %2 seconds.").arg(dlg.actionName).arg(dlg.secondsLeft)
    }

    footer: Item {
        implicitHeight: 64
        RowLayout {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Button { text: qsTr("Cancel"); onClicked: { ticker.stop(); App.whenDone = 0; dlg.close() } }
            Button { text: qsTr("Now"); highlighted: true; onClicked: { ticker.stop(); dlg.close(); App.performWhenDone(dlg.action) } }
        }
    }
}
