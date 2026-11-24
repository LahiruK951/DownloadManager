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
    readonly property bool stream: App.linkModule(urlsField.text) === "downloadsm3u"
    readonly property bool dash: App.linkModule(urlsField.text) === "downloadsfragments"
    readonly property bool site: App.linkModule(urlsField.text) === "downloadsms"
    property bool folderTouched: false
    // The name is already taken in the chosen folder: let the user decide here and now.
    readonly property bool nameTaken: linkCount === 1 && !torrent && App.fileExistsIn(folderField.editText, nameField.text !== "" ? nameField.text : App.expectedName(urlsField.text))
    property int existingChoice: 0 // 0 keep both, 1 replace
    // The link is already in the list: say so (adding again is still allowed).
    readonly property var duplicate: linkCount > 0 ? App.duplicateOf(urlsField.text) : ({})
    // A paused or failed download of the same file: offer to continue it instead of starting over.
    readonly property var unfinished: linkCount === 1 && !torrent ? App.unfinishedMatch(urlsField.text, nameField.text) : ({})
    readonly property bool canContinue: unfinished.id !== undefined
    readonly property string autoFolder: App.folderFor(urlsField.text)
    onAutoFolderChanged: if (!folderTouched && visible) folderField.editText = autoFolder

    function openWith(text, folder) { // folder: optional, else the best one for the link
        urlsField.text = text || ""
        nameField.text = App.suggestedName(text || "")
        hashField.text = ""
        errorText.text = ""
        startBox.checked = true
        existingChoice = 0
        const s = App.defaultSchedule()
        schedBox.checked = false
        schedEditor.days = s.days
        schedEditor.fromMin = s.from
        schedEditor.toMin = s.to
        folderField.model = App.folderHistory // taken once: a later change mustn't reset the folder typed here
        open()
        folderField.editText = folder || App.folderFor(text || "")
        folderTouched = false
        urlsField.area.forceActiveFocus()
        urlsField.area.cursorPosition = urlsField.area.length
    }

    function submit() {
        const err = App.addDownloads(urlsField.text, folderField.editText, nameField.text, startBox.checked,
                                     { enabled: schedBox.checked, days: schedEditor.days, from: schedEditor.fromMin, to: schedEditor.toMin },
                                     nameTaken ? existingChoice : -1, hashField.visible ? hashField.text : "")
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
                     : dlg.stream ? qsTr("Video stream (HLS) — saved as one video file")
                     : dlg.dash ? qsTr("Video stream (DASH) — best video and audio, combined into one file")
                     : dlg.site ? (App.ytdlpFound() !== "" ? qsTr("Video from a website — best quality, through yt-dlp")
                                                          : qsTr("Video from a website — needs yt-dlp (Settings > Downloads)"))
                     : dlg.linkCount === 1 ? qsTr("1 file") : qsTr("%1 files").arg(dlg.linkCount)
            }
            FButton {
                kind: "ghost"
                glyph: Theme.icons.folder
                text: qsTr("Browse…")
                implicitHeight: 28
                onClicked: fileDlg.open()
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
                onActivated: dlg.folderTouched = true
                onAccepted: dlg.folderTouched = true
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
        FTextField {
            id: hashField
            Layout.fillWidth: true
            Layout.topMargin: 6
            visible: dlg.linkCount === 1 && !dlg.torrent
            placeholderText: qsTr("Checksum to verify (optional): MD5, SHA-1 or SHA-256")
            onAccepted: dlg.submit()
        }
        RowLayout {
            Layout.fillWidth: true
            visible: dlg.nameTaken
            spacing: 8
            Text {
                text: Theme.icons.warn
                font.family: Theme.iconFont
                font.pixelSize: 12
                color: Theme.warning
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("A file with this name is already in the folder.")
                elide: Text.ElideRight
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
            FSegmented {
                itemHeight: 22
                model: [qsTr("Keep both"), qsTr("Replace")]
                currentIndex: dlg.existingChoice
                onActivated: (i) => dlg.existingChoice = i
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: dlg.canContinue
            spacing: 8
            Text {
                text: Theme.icons.refresh
                font.family: Theme.iconFont
                font.pixelSize: 12
                color: Theme.accent
            }
            Text {
                Layout.fillWidth: true
                readonly property string what: dlg.unfinished.percent >= 0 ? "%1 (%2%)".arg(dlg.unfinished.title || "").arg(dlg.unfinished.percent)
                                                                         : (dlg.unfinished.title || "")
                text: dlg.unfinished.exact ? qsTr("Unfinished in your list: %1").arg(what)
                                           : qsTr("Looks like the unfinished %1").arg(what)
                elide: Text.ElideMiddle
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
            FButton {
                text: qsTr("Show")
                onClicked: { App.select(dlg.unfinished.id); dlg.close() }
            }
            FButton {
                kind: "primary"
                text: qsTr("Continue it")
                onClicked: {
                    const err = App.continueWith(dlg.unfinished.id, urlsField.text.trim())
                    if (err === "") dlg.close()
                    else errorText.text = err
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: dlg.duplicate.count > 0 && !dlg.canContinue
            spacing: 8
            Text {
                text: Theme.icons.warn
                font.family: Theme.iconFont
                font.pixelSize: 12
                color: Theme.warning
            }
            Text {
                Layout.fillWidth: true
                text: dlg.duplicate.count > 1 ? qsTr("%1 of these links are already in your list.").arg(dlg.duplicate.count)
                                              : qsTr("Already in your list: %1 (%2)").arg(dlg.duplicate.title || "").arg(dlg.duplicate.state || "")
                elide: Text.ElideRight
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
            FButton {
                text: qsTr("Show")
                visible: dlg.duplicate.count === 1
                onClicked: { App.select(dlg.duplicate.id); dlg.close() }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.topMargin: 12; height: 1; color: Theme.border }

        FCheckBox {
            id: startBox
            Layout.topMargin: 4
            text: qsTr("Start downloading immediately")
            checked: true
            enabled: !schedBox.checked
        }
        FCheckBox { id: schedBox; text: qsTr("Schedule download") }
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
            kind: dlg.canContinue ? "secondary" : "primary" // "Continue it" is the better choice then
            glyph: Theme.icons.down
            text: dlg.linkCount > 1 ? qsTr("Download %1 files").arg(dlg.linkCount) : qsTr("Download")
            enabled: dlg.linkCount > 0
            onClicked: dlg.submit()
        }
    ]

    FileDialog { // a .torrent or Metalink file already on this computer
        id: fileDlg
        title: qsTr("Choose a file to download from")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("Torrent and Metalink files (*.torrent *.metalink *.meta4)"), qsTr("All files (*)")]
        onAccepted: {
            const paths = selectedFiles.map(u => App.urlToLocal(u)).join("\n")
            const cur = urlsField.text.trim()
            urlsField.text = cur === "" ? paths : cur + "\n" + paths
            if (!dlg.folderTouched) folderField.editText = App.folderFor(urlsField.text)
        }
    }

    FolderDialog {
        id: folderDlg
        currentFolder: App.toUrl(folderField.editText)
        onAccepted: { folderField.editText = App.urlToLocal(selectedFolder); dlg.folderTouched = true }
    }
}
