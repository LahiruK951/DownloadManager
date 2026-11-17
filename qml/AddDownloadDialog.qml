import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: dlg
    title: qsTr("New download")
    modal: true
    anchors.centerIn: parent
    width: Math.min(640, parent.width - 40)
    padding: 20

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
        urlsField.forceActiveFocus()
        urlsField.cursorPosition = urlsField.length
    }

    function submit() {
        const err = App.addDownloads(urlsField.text, folderField.editText, nameField.text, startBox.checked,
                                     { enabled: schedBox.checked, days: schedEditor.days, from: schedEditor.fromMin, to: schedEditor.toMin })
        if (err === "") close()
        else errorText.text = err
    }

    background: Rectangle { color: Theme.surface; radius: 10; border.color: Theme.border }
    header: Label {
        text: dlg.title
        color: Theme.text
        font.pixelSize: 17
        padding: 20
        bottomPadding: 0
    }

    contentItem: ColumnLayout {
        spacing: 8

                Label {
            text: qsTr("Link, magnet or .torrent (one per line; ranges like file[1-20].zip expand)")
            color: Theme.textDim
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 84
            TextArea {
                id: urlsField
                wrapMode: TextArea.WrapAnywhere
                placeholderText: "https://example.com/file.zip   magnet:?xt=urn:btih:…"
                color: Theme.text
                placeholderTextColor: Theme.textDisabled
                selectByMouse: true
                background: Rectangle { color: Theme.surfaceAlt; radius: 6; border.color: urlsField.activeFocus ? Theme.accent : Theme.border }
                Keys.onReturnPressed: (event) => {
                    if (event.modifiers & Qt.ControlModifier) dlg.submit()
                    else event.accepted = false
                }
            }
        }

        Label { text: qsTr("Save to"); color: Theme.textDim; Layout.topMargin: 6 }
        RowLayout {
            Layout.fillWidth: true
            ComboBox {
                id: folderField
                Layout.fillWidth: true
                editable: true
                model: App.folderHistory
            }
            Button { text: qsTr("Browse…"); onClicked: folderDlg.open() }
        }

        Label { text: qsTr("File name (optional)"); color: Theme.textDim; Layout.topMargin: 6; visible: dlg.linkCount <= 1 && !dlg.torrent }
        TextField {
            id: nameField
            Layout.fillWidth: true
            visible: dlg.linkCount <= 1 && !dlg.torrent
            placeholderText: qsTr("Taken from the server")
            color: Theme.text
            placeholderTextColor: Theme.textDisabled
            selectByMouse: true
            onAccepted: dlg.submit()
        }

        CheckBox { id: startBox; text: qsTr("Start downloading immediately"); checked: true; Layout.topMargin: 4; enabled: !schedBox.checked }
        CheckBox { id: schedBox; text: qsTr("Schedule: download only during this time") }
        ScheduleEditor { id: schedEditor; visible: schedBox.checked; Layout.leftMargin: 28 }

        Text {
            id: errorText
            Layout.fillWidth: true
            visible: text !== ""
            color: Theme.error
            wrapMode: Text.WordWrap
        }
    }

    footer: Item {
        implicitHeight: 64
        RowLayout {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Button { text: qsTr("Cancel"); onClicked: dlg.close() }
            Button {
                text: dlg.linkCount > 1 ? qsTr("Download %1 files").arg(dlg.linkCount) : qsTr("Download")
                highlighted: true
                enabled: dlg.linkCount > 0
                onClicked: dlg.submit()
            }
        }
    }

    FolderDialog {
        id: folderDlg
        currentFolder: App.toUrl(folderField.editText)
        onAccepted: folderField.editText = App.urlToLocal(selectedFolder)
    }
}
