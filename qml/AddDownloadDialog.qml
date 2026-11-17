import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

FDialog {
    id: dlg
    title: qsTr("New download")
    width: Math.min(620, parent.width - 48)

    readonly property int linkCount: App.countLinks(urlsField.text)
    readonly property bool torrent: App.isTorrentLink(urlsField.text)

    function openWith(text) {
        urlsField.text = text || ""
        nameField.text = ""
        errorText.text = ""
        startBox.checked = true
        const s = App.defaultSchedule()
        schedBox.checked = false
        schedEditor.days = s.days
        schedEditor.fromMin = s.from
        schedEditor.toMin = s.to
        open()
        folderField.editText = App.defaultFolder
        urlsField.area.forceActiveFocus()
        urlsField.area.cursorPosition = urlsField.area.length
    }

    function submit() {
        const err = App.addDownloads(urlsField.text, folderField.editText, nameField.text, startBox.checked,
                                     { enabled: schedBox.checked, days: schedEditor.days, from: schedEditor.fromMin, to: schedEditor.toMin })
        if (err === "") close()
        else errorText.text = err
    }

    contentItem: ColumnLayout {
        spacing: 6

        FieldLabel { text: qsTr("Link") }
        FTextArea {
            id: urlsField
            Layout.fillWidth: true
            Layout.preferredHeight: 92
            placeholderText: qsTr("Paste a link, magnet or .torrent path — one per line")
            area.Keys.onReturnPressed: (event) => {
                if (event.modifiers & Qt.ControlModifier) dlg.submit()
                else event.accepted = false
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Text {
                visible: dlg.linkCount > 0
                text: dlg.torrent ? Theme.icons.link : Theme.icons.down
                font.family: Theme.iconFont
                font.pixelSize: 11
                color: Theme.accent
            }
            Text {
                Layout.fillWidth: true
                font.pixelSize: Theme.fontSmall
                color: dlg.linkCount > 0 ? Theme.accent : Theme.textDim
                text: dlg.linkCount === 0 ? qsTr("Tip: ranges like file[1-20].zip expand into several downloads")
                     : dlg.torrent ? qsTr("BitTorrent download")
                     : dlg.linkCount === 1 ? qsTr("1 file") : qsTr("%1 files").arg(dlg.linkCount)
            }
        }

        FieldLabel { text: qsTr("Save to"); Layout.topMargin: 8 }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            FComboBox {
                id: folderField
                Layout.fillWidth: true
                editable: true
                model: App.folderHistory
            }
            FButton { text: qsTr("Browse…"); onClicked: folderDlg.open() }
        }

        FieldLabel {
            text: qsTr("File name")
            Layout.topMargin: 8
            visible: dlg.linkCount <= 1 && !dlg.torrent
        }
        FTextField {
            id: nameField
            Layout.fillWidth: true
            visible: dlg.linkCount <= 1 && !dlg.torrent
            placeholderText: qsTr("Automatic (from the server)")
            onAccepted: dlg.submit()
        }

        Rectangle { Layout.fillWidth: true; Layout.topMargin: 12; height: 1; color: Theme.border }

        FCheckBox {
            id: startBox
            Layout.topMargin: 4
            text: qsTr("Start downloading immediately")
            checked: true
            enabled: !schedBox.checked
        }
        FCheckBox { id: schedBox; text: qsTr("Download only during a scheduled time") }
        ScheduleEditor {
            id: schedEditor
            visible: schedBox.checked
            Layout.leftMargin: 32
            Layout.bottomMargin: 4
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 6
            visible: errorText.text !== ""
            implicitHeight: errorText.implicitHeight + 16
            radius: Theme.radius
            color: Theme.errorSoft
            Text {
                id: errorText
                anchors.fill: parent
                anchors.margins: 8
                color: Theme.error
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            glyph: Theme.icons.down
            text: dlg.linkCount > 1 ? qsTr("Download %1 files").arg(dlg.linkCount) : qsTr("Download")
            enabled: dlg.linkCount > 0
            onClicked: dlg.submit()
        }
    ]

    FolderDialog {
        id: folderDlg
        currentFolder: App.toUrl(folderField.editText)
        onAccepted: folderField.editText = App.urlToLocal(selectedFolder)
    }
}
