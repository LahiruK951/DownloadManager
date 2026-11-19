import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// MP3 / MP4 converter for a finished audio or video download (FDM: "Convert").
FDialog {
    id: dlg
    property var target: null
    property string format: "mp3"
    readonly property bool mp4Possible: target ? App.canConvert(target.id, "mp4") : false
    readonly property var bitrates: [128, 160, 192, 256, 320]
    readonly property var qualities: [0, 2, 4, 6] // LAME -V levels
    title: qsTr("Convert")
    subtitle: target ? target.title : ""
    width: Math.min(500, parent.width - 48)

    function openFor(d, fmt) {
        target = d
        format = fmt === "mp4" && App.canConvert(d.id, "mp4") ? "mp4" : "mp3"
        load()
        open()
    }
    function load() {
        const o = App.convertDefaults(format)
        modeSeg.currentIndex = o.vbr ? 1 : 0
        bitrateBox.currentIndex = Math.max(0, bitrates.indexOf(o.bitrate))
        qualityBox.currentIndex = Math.max(0, qualities.indexOf(o.quality))
        folderField.text = o.folder
    }
    function start() {
        App.convertFile(target.id, format, {
            vbr: modeSeg.currentIndex === 1,
            bitrate: bitrates[bitrateBox.currentIndex],
            quality: qualities[qualityBox.currentIndex],
            folder: folderField.text
        })
        close()
    }

    contentItem: ColumnLayout {
        spacing: 8
        FSegmented {
            model: [qsTr("MP3 audio"), qsTr("MP4 video")]
            currentIndex: dlg.format === "mp4" ? 1 : 0
            onActivated: (i) => {
                if (i === 1 && !dlg.mp4Possible) return
                dlg.format = i === 1 ? "mp4" : "mp3"
                dlg.load()
            }
            opacity: 1
        }
        Text {
            visible: !dlg.mp4Possible
            text: qsTr("MP4 needs a video that isn't MP4 already.")
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
        }

        // ---------------- MP3 options
        FieldLabel { text: qsTr("Bitrate"); visible: dlg.format === "mp3"; Layout.topMargin: 6 }
        RowLayout {
            visible: dlg.format === "mp3"
            spacing: 10
            FSegmented {
                id: modeSeg
                model: [qsTr("Constant"), qsTr("Variable")]
                onActivated: (i) => currentIndex = i
            }
            FComboBox {
                id: bitrateBox
                visible: modeSeg.currentIndex === 0
                Layout.preferredWidth: 140
                model: dlg.bitrates.map(b => b + " kbps")
            }
            FComboBox {
                id: qualityBox
                visible: modeSeg.currentIndex === 1
                Layout.preferredWidth: 220
                model: [qsTr("Best (about 245 kbps)"), qsTr("High (about 190 kbps)"), qsTr("Standard (about 165 kbps)"), qsTr("Small (about 115 kbps)")]
            }
        }

        // ---------------- MP4 note
        Text {
            visible: dlg.format === "mp4"
            Layout.fillWidth: true
            Layout.topMargin: 6
            wrapMode: Text.Wrap
            text: qsTr("Takes seconds when the video and audio already fit MP4 (no quality loss). Otherwise they are re-encoded to H.264 and AAC, which takes longer.")
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
        }

        FieldLabel { text: qsTr("Save to"); Layout.topMargin: 6 }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            FTextField {
                id: folderField
                Layout.fillWidth: true
                placeholderText: qsTr("Same folder as the original")
            }
            FButton { text: qsTr("Browse…"); onClicked: folderDlg.open() }
        }
        Text {
            Layout.fillWidth: true
            text: App.ffmpegFound() !== "" ? qsTr("The original file is kept.")
                                          : qsTr("ffmpeg was not found. Install it or set its location in Settings > Downloads.")
            color: App.ffmpegFound() !== "" ? Theme.textDim : Theme.error
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSmall
        }
    }

    FolderDialog {
        id: folderDlg
        onAccepted: folderField.text = App.urlToLocal(selectedFolder)
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            text: dlg.format === "mp4" ? qsTr("Convert to MP4") : qsTr("Convert to MP3")
            enabled: App.ffmpegFound() !== ""
            onClicked: dlg.start()
        }
    ]
}
