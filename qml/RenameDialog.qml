import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Rename a finished download's file on disk (context menu "Rename…", F2).
FDialog {
    id: dlg
    property var target: null
    property string error: ""
    title: qsTr("Rename")
    subtitle: target ? target.destDir : ""
    width: Math.min(520, parent.width - 48)

    function openFor(d) {
        target = d
        error = ""
        nameField.text = d.title
        open()
        nameField.forceActiveFocus()
        // Like Explorer: select the name, keep the extension.
        const dot = d.title.lastIndexOf(".")
        nameField.select(0, dot > 0 ? dot : d.title.length)
    }

    function apply() {
        const err = App.rename(target.id, nameField.text)
        if (err === "") dlg.close()
        else error = err
    }

    contentItem: ColumnLayout {
        spacing: 8
        FTextField {
            id: nameField
            Layout.fillWidth: true
            onTextEdited: dlg.error = ""
            onAccepted: dlg.apply()
        }
        Text {
            Layout.fillWidth: true
            visible: dlg.error !== ""
            text: dlg.error
            wrapMode: Text.Wrap
            color: Theme.error
            font.pixelSize: Theme.fontSmall
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            text: qsTr("Rename")
            enabled: nameField.text.trim() !== "" && dlg.target && nameField.text.trim() !== dlg.target.title
            onClicked: dlg.apply()
        }
    ]
}
