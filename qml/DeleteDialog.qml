import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FDialog {
    id: dlg
    property var target: null
    property int multiCount: 0
    title: multiCount > 1 ? qsTr("Delete %1 downloads?").arg(multiCount) : qsTr("Delete download?")
    subtitle: multiCount > 1 ? "" : (target ? target.title : "")

    function openForSelection() {
        if (App.selectionCount <= 1) {
            openFor(App.selected)
            return
        }
        target = null
        multiCount = App.selectionCount
        open()
    }
    width: Math.min(460, parent.width - 48)

    function openFor(d) {
        multiCount = 0
        target = d
        if (d)
            open()
    }

    contentItem: Text {
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: Theme.fontBody
        text: dlg.multiCount > 1 ? qsTr("Remove them from the list only, or also delete their files from disk. Unfinished downloads lose their partial data either way.")
             : !dlg.target ? ""
             : dlg.target.completed
               ? qsTr("Remove it from the list only, or also delete the downloaded file from disk.")
               : qsTr("The download isn't finished. Its partial data will be removed.")
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            text: qsTr("Remove from list")
            kind: dlg.multiCount > 1 || (dlg.target && dlg.target.completed) ? "secondary" : "danger"
            onClicked: {
                if (dlg.multiCount > 1) App.removeSelected(false)
                else App.remove(dlg.target.id, false)
                dlg.close()
            }
        },
        FButton {
            kind: "danger"
            glyph: Theme.icons.del
            text: dlg.multiCount > 1 ? qsTr("Delete files") : qsTr("Delete file")
            visible: dlg.multiCount > 1 || (dlg.target !== null && dlg.target.completed)
            onClicked: {
                if (dlg.multiCount > 1) App.removeSelected(true)
                else App.remove(dlg.target.id, true)
                dlg.close()
            }
        }
    ]
}
