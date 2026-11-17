import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FDialog {
    id: dlg
    property var target: null
    title: qsTr("Delete download?")
    subtitle: target ? target.title : ""
    width: Math.min(460, parent.width - 48)

    function openFor(d) {
        target = d
        if (d)
            open()
    }

    contentItem: Text {
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontBody
        text: !dlg.target ? ""
             : dlg.target.completed
               ? qsTr("Remove it from the list only, or also delete the downloaded file from disk.")
               : qsTr("The download isn't finished. Its partial data will be removed.")
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            text: qsTr("Remove from list")
            kind: dlg.target && dlg.target.completed ? "secondary" : "danger"
            onClicked: { App.remove(dlg.target.id, false); dlg.close() }
        },
        FButton {
            kind: "danger"
            glyph: Theme.icons.del
            text: qsTr("Delete file")
            visible: dlg.target !== null && dlg.target.completed
            onClicked: { App.remove(dlg.target.id, true); dlg.close() }
        }
    ]
}
