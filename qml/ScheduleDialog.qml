import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Per-download schedule (FDM: "Schedule…" in the download's context menu).
Dialog {
    id: dlg
    property var target: null
    title: qsTr("Schedule download")
    modal: true
    anchors.centerIn: parent
    width: Math.min(560, parent.width - 40)
    padding: 20

    function openFor(d) {
        target = d
        const s = App.downloadSchedule(d.id)
        enabledBox.checked = s.enabled
        editor.days = s.days
        editor.fromMin = s.from
        editor.toMin = s.to
        open()
    }

    background: Rectangle { color: Theme.surface; radius: 10; border.color: Theme.border }
    header: Label { text: dlg.title; color: Theme.text; font.pixelSize: 17; padding: 20; bottomPadding: 0 }

    contentItem: ColumnLayout {
        spacing: 10
        Text {
            Layout.fillWidth: true
            text: dlg.target ? dlg.target.title : ""
            elide: Text.ElideMiddle
            color: Theme.textDim
        }
        CheckBox { id: enabledBox; text: qsTr("Download only during this time") }
        ScheduleEditor { id: editor; enabled: enabledBox.checked }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 12
            text: qsTr("Outside the window the download waits and resumes automatically. Use the same time for both ends to allow the whole day.")
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
                text: qsTr("Save")
                highlighted: true
                onClicked: {
                    App.setDownloadSchedule(dlg.target.id, enabledBox.checked,
                                            { days: editor.days, from: editor.fromMin, to: editor.toMin })
                    dlg.close()
                }
            }
        }
    }
}
