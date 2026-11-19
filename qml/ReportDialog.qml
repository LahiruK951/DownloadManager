import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// "Report a problem": shows exactly what would be shared (version, Windows version, crash
// files, recent log). Nothing is sent anywhere: the user copies or saves it.
FDialog {
    id: dlg
    property bool afterCrash: false
    property string text: ""
    title: afterCrash ? qsTr("FluxDM closed unexpectedly") : qsTr("Report a problem")
    subtitle: afterCrash ? qsTr("Sorry about that. This report can help find out why.") : ""
    width: Math.min(720, parent.width - 48)

    function openReport(crash) {
        afterCrash = crash === true
        text = App.problemReport()
        open()
    }

    contentItem: ColumnLayout {
        spacing: 8
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
            text: qsTr("Nothing is sent automatically. The report includes your recent download log (links and file names), so look through it before you share it. Crash files (.dmp) are in the logs folder; attach them too if there are any.")
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 300
            radius: Theme.radius
            color: Theme.inputBg
            border.color: Theme.border
            ScrollView {
                anchors.fill: parent
                anchors.margins: 1
                TextArea {
                    readOnly: true
                    text: dlg.text
                    wrapMode: TextEdit.NoWrap
                    font.family: "Consolas"
                    font.pixelSize: 11
                    color: Theme.text
                    selectByMouse: true
                    background: null
                }
            }
        }
    }

    FileDialog {
        id: saveDlg
        fileMode: FileDialog.SaveFile
        defaultSuffix: "txt"
        currentFile: App.toUrl(App.defaultFolder + "/fluxdm-report.txt")
        nameFilters: [qsTr("Text files (*.txt)")]
        onAccepted: App.saveProblemReport(selectedFile)
    }

    buttons: [
        FButton { kind: "ghost"; glyph: Theme.icons.folder; text: qsTr("Open logs folder"); onClicked: App.openLogsFolder() },
        FButton { text: qsTr("Save…"); onClicked: saveDlg.open() },
        FButton { text: qsTr("Copy"); onClicked: App.copyProblemReport() },
        FButton { kind: "primary"; text: qsTr("Close"); onClicked: dlg.close() }
    ]
}
