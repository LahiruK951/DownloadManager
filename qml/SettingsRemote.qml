import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings > Remote: remote control from the local network.
ColumnLayout {
    spacing: 20
    SettingsCard {
        title: qsTr("Remote control")
        description: qsTr("Check and add downloads from your phone or another computer on the same network.")
        Layout.fillWidth: true
        SettingRow {
            title: qsTr("Allow remote control")
            description: qsTr("Only devices on your local network can connect, and only with the access code.")
            FToggle { checked: App.remoteEnabled; onToggled: App.remoteEnabled = checked }
        }
        SettingRow {
            title: qsTr("Port")
            FSpinBox {
                implicitWidth: 130
                from: 1024; to: 65535
                Component.onCompleted: value = App.remotePort
                onValueModified: App.remotePort = value
            }
        }
        SettingRow {
            title: qsTr("Access code")
            description: qsTr("Making a new one signs out every device.")
            divider: false
            RowLayout {
                spacing: 10
                Text {
                    text: App.remoteCode
                    color: Theme.text
                    font.pixelSize: 20
                    font.letterSpacing: 3
                    font.family: "Consolas"
                }
                FButton { kind: "ghost"; text: qsTr("New code"); onClicked: App.newRemoteCode() }
            }
        }
    }
    SettingsCard {
        visible: App.remoteEnabled
        title: qsTr("Open on your phone")
        Layout.fillWidth: true
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: App.remoteError !== "" ? Theme.error : Theme.text
            font.pixelSize: Theme.fontBody
            text: App.remoteError !== "" ? App.remoteError
                  : App.remoteAddresses.length ? App.remoteAddresses.join("\n")
                  : qsTr("No network connection found.")
        }
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 8
            wrapMode: Text.Wrap
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
            text: qsTr("If Windows asks whether FluxDM may use the network, allow private networks. The connection isn't encrypted, so use it only on networks you trust.")
        }
    }
}
