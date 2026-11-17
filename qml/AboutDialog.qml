import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

FDialog {
    id: dlg
    title: qsTr("About")
    width: Math.min(420, parent.width - 48)

    contentItem: ColumnLayout {
        spacing: 10
        RowLayout {
            spacing: 14
            Rectangle {
                implicitWidth: 52
                implicitHeight: 52
                radius: 12
                color: Theme.accent
                Text {
                    anchors.centerIn: parent
                    text: Theme.icons.down
                    font.family: Theme.iconFont
                    font.pixelSize: 24
                    color: "white"
                }
            }
            Column {
                spacing: 2
                Text { text: "FluxDM"; color: Theme.text; font.pixelSize: 20; font.weight: Font.DemiBold }
                Text { text: qsTr("Version %1").arg(App.version); color: Theme.textDim; font.pixelSize: Theme.fontSmall }
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 6
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.pixelSize: Theme.fontBody
            text: qsTr("Multi-connection HTTP/FTP and BitTorrent download manager, built with Qt, libcurl and libtorrent.")
        }
    }

    buttons: [
        FButton { kind: "primary"; text: qsTr("Close"); onClicked: dlg.close() }
    ]
}
