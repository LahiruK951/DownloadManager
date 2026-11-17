import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Per-download schedule (FDM: "Schedule…" in the download's context menu).
FDialog {
    id: dlg
    property var target: null
    title: qsTr("Schedule download")
    subtitle: target ? target.title : ""
    width: Math.min(540, parent.width - 48)

    function openFor(d) {
        target = d
        const s = App.downloadSchedule(d.id)
        enabledBox.checked = s.enabled
        editor.days = s.days
        editor.fromMin = s.from
        editor.toMin = s.to
        open()
    }

    contentItem: ColumnLayout {
        spacing: 10
        FSwitch {
            id: enabledBox
            Layout.fillWidth: true
            text: qsTr("Download only during this time")
            description: qsTr("Outside the window the download waits and resumes automatically.")
        }
        ScheduleEditor { id: editor; enabled: enabledBox.checked; opacity: enabled ? 1 : 0.5 }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
            text: qsTr("Tip: use the same start and end time to allow the whole day. Windows may cross midnight (e.g. 23:00 – 07:00).")
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            text: qsTr("Save")
            onClicked: {
                App.setDownloadSchedule(dlg.target.id, enabledBox.checked,
                                        { days: editor.days, from: editor.fromMin, to: editor.toMin })
                dlg.close()
            }
        }
    ]
}
