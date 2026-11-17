import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dlg
    title: qsTr("About FluxDM")
    modal: true
    anchors.centerIn: parent
    width: Math.min(420, parent.width - 40)
    padding: 24
    standardButtons: Dialog.Close

    background: Rectangle { color: Theme.surface; radius: 10; border.color: Theme.border }

    contentItem: ColumnLayout {
        spacing: 8
        Text { text: "FluxDM " + App.version; color: Theme.text; font.pixelSize: 20 }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            text: qsTr("A multi-connection download manager built with Qt %1 and libcurl.").arg("6")
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: 12
            text: qsTr("Data folder: settings.ini and db.sqlite live next to each other in your local app data.")
        }
    }
}
