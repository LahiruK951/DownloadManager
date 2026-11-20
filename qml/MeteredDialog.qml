import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The connection became metered (phone hotspot, data limit) while downloads are running.
FDialog {
    id: dlg
    property int active: 0
    title: qsTr("Metered connection")
    width: Math.min(480, parent.width - 48)

    function ask(count) {
        active = count
        remember.checked = false
        open()
    }

    contentItem: ColumnLayout {
        spacing: 10
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.fontBody
            text: qsTr("Windows says this connection is metered, for example a phone hotspot or a data plan with a limit. %1 download(s) would keep using it.").arg(dlg.active)
        }
        FCheckBox { id: remember; text: qsTr("Don't ask again") }
    }

    buttons: [
        FButton {
            text: qsTr("Keep downloading")
            onClicked: { App.keepDownloadingOnMetered(remember.checked); dlg.close() }
        },
        FButton {
            kind: "primary"
            text: qsTr("Pause downloads")
            onClicked: { App.pauseOnMetered(remember.checked); dlg.close() }
        }
    ]
}
