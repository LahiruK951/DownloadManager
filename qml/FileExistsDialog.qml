import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "The file already exists": shown when a download set to "Ask" finds its name taken.
FDialog {
    id: dlg
    property var target: null
    title: qsTr("File already exists")
    subtitle: target ? target.destDir : ""
    width: Math.min(500, parent.width - 48)

    function openFor(d) {
        target = d
        open()
    }

    contentItem: ColumnLayout {
        spacing: 8
        Text {
            Layout.fillWidth: true
            text: dlg.target ? qsTr("“%1” is already in this folder. Replace it with the new download, or keep both?").arg(dlg.target.title) : ""
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.fontBody
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("The existing file is only replaced once the new one has finished downloading.")
            wrapMode: Text.Wrap
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton { text: qsTr("Keep both"); onClicked: { App.resolveExisting(dlg.target.id, 0); dlg.close() } },
        FButton { kind: "primary"; text: qsTr("Replace"); onClicked: { App.resolveExisting(dlg.target.id, 1); dlg.close() } }
    ]
}
