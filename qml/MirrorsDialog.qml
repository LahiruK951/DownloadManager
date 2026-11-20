import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Other addresses of the same file (FDM: mirrors). Connections are spread over them; a mirror
// that fails or serves a different file is dropped for that download.
FDialog {
    id: dlg
    property var target: null
    property string error: ""
    title: qsTr("Mirrors")
    subtitle: target ? target.title : ""
    width: Math.min(560, parent.width - 48)

    function openFor(d) {
        target = d
        area.text = App.mirrors(d.id)
        error = ""
        open()
    }

    contentItem: ColumnLayout {
        spacing: 8
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
            text: qsTr("Other links to exactly the same file, one per line. FluxDM downloads parts of it from each; a mirror that fails or has a different file is skipped.")
        }
        FTextArea {
            id: area
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            placeholderText: "https://mirror.example.org/file.iso"
        }
        Text {
            Layout.fillWidth: true
            visible: dlg.error !== ""
            text: dlg.error
            color: Theme.error
            font.pixelSize: Theme.fontSmall
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            text: qsTr("Save")
            onClicked: {
                dlg.error = App.setMirrors(dlg.target.id, area.text)
                if (dlg.error === "") dlg.close()
            }
        }
    ]
}
