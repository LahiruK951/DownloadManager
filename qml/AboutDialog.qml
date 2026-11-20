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

        FieldLabel { text: qsTr("Keyboard shortcuts"); Layout.topMargin: 6 }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3
            Repeater {
                model: [
                    ["Ctrl+N, Ctrl+V", qsTr("New download (from the clipboard)")],
                    ["Space", qsTr("Pause or resume the selected download")],
                    ["Enter", qsTr("Open the file")],
                    ["Ctrl+E", qsTr("Show in folder")],
                    ["F2", qsTr("Rename")],
                    ["Del", qsTr("Delete")],
                    ["Ctrl+Shift+P / R", qsTr("Pause all / resume all")],
                    ["Ctrl+1 / 2 / 3", qsTr("Traffic mode Snail / Medium / High")],
                    ["Ctrl+I", qsTr("Show or hide details")],
                    ["Ctrl+F", qsTr("Search")],
                    ["Ctrl+A, Esc", qsTr("Select all, clear selection")],
                    ["Ctrl+,", qsTr("Settings")]
                ]
                delegate: Item {
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: row.implicitHeight
                    RowLayout {
                        id: row
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 16
                        Text {
                            Layout.preferredWidth: 120
                            text: modelData[0]
                            color: Theme.text
                            font.pixelSize: Theme.fontSmall
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData[1]
                            color: Theme.textDim
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }

    buttons: [
        FButton { kind: "primary"; text: qsTr("Close"); onClicked: dlg.close() }
    ]
}
