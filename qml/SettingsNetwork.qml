import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings > Network: proxy and connections.
ColumnLayout {
    id: netPage
    spacing: 20
    property int mode: 0
    property string type: "http"
    property bool hasPassword: false
    property string status: ""
    property bool statusError: false
    function load() {
        const p = App.proxySettings()
        mode = p.mode
        type = p.type
        hostField.text = p.host
        portField.value = p.port
        userField.text = p.user
        passField.text = ""
        hasPassword = p.hasPassword
        status = ""
    }
    function apply() {
        const err = App.setProxySettings(mode, type, hostField.text, portField.value, userField.text, passField.text, true)
        statusError = err !== ""
        status = err !== "" ? err : qsTr("Saved. New connections use it; running downloads switch when they reconnect.")
        if (err === "") hasPassword = hasPassword || passField.text !== ""
    }
    onVisibleChanged: if (visible) load()

    SettingsCard {
        title: qsTr("Proxy")
        description: qsTr("For web downloads, video streams and .torrent files.")
        Layout.fillWidth: true
        SettingRow {
            title: qsTr("Connect")
            divider: netPage.mode === 2
            FSegmented {
                model: [qsTr("System proxy"), qsTr("No proxy"), qsTr("Manual")]
                currentIndex: netPage.mode
                onActivated: (i) => { netPage.mode = i; if (i !== 2) netPage.apply() }
            }
        }
        ColumnLayout {
            visible: netPage.mode === 2
            Layout.fillWidth: true
            Layout.topMargin: 8
            spacing: 6
            RowLayout {
                spacing: 10
                FSegmented {
                    model: ["HTTP", "SOCKS5"]
                    currentIndex: netPage.type === "socks5" ? 1 : 0
                    onActivated: (i) => netPage.type = i === 1 ? "socks5" : "http"
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    FieldLabel { text: qsTr("Address") }
                    FTextField { id: hostField; Layout.fillWidth: true; placeholderText: qsTr("proxy.example.com") }
                }
                ColumnLayout {
                    spacing: 2
                    FieldLabel { text: qsTr("Port") }
                    FSpinBox { id: portField; implicitWidth: 130; from: 1; to: 65535; value: 8080 }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    FieldLabel { text: qsTr("User name (optional)") }
                    FTextField { id: userField; Layout.fillWidth: true }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    FieldLabel { text: qsTr("Password") }
                    FTextField {
                        id: passField
                        Layout.fillWidth: true
                        echoMode: TextInput.Password
                        placeholderText: netPage.hasPassword ? qsTr("Saved (unchanged)") : ""
                        enabled: userField.text.trim() !== ""
                    }
                }
            }
            RowLayout {
                Layout.topMargin: 6
                spacing: 10
                FButton { kind: "primary"; text: qsTr("Save proxy"); onClicked: netPage.apply() }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: netPage.status !== ""
        text: netPage.status
        color: netPage.statusError ? Theme.error : Theme.textDim
        wrapMode: Text.Wrap
        font.pixelSize: Theme.fontSmall
    }
    SettingsCard {
        title: qsTr("Connections")
        Layout.fillWidth: true
        SettingRow {
            title: qsTr("On metered connections")
            description: qsTr("Phone hotspots or data plans with a limit, as Windows reports them.")
            FComboBox {
                model: [qsTr("Ask me"), qsTr("Pause downloads"), qsTr("Download as usual")]
                currentIndex: App.meteredPolicy
                onActivated: (i) => App.meteredPolicy = i
            }
        }
        SettingRow {
            id: trustRow
            readonly property int count: (App.defaultFolder, App.trustedHostCount())
            title: qsTr("Servers trusted despite their certificate")
            description: count === 0 ? qsTr("None.") : qsTr("%1 server(s). FluxDM doesn't check their certificates.").arg(count)
            FButton { text: qsTr("Forget all"); enabled: trustRow.count > 0; onClicked: App.forgetTrustedHosts() }
        }
        SettingRow {
            title: qsTr("Internet protocol")
            description: qsTr("Some networks have a broken IPv6 (or IPv4) route; pick the one that works.")
            FSegmented {
                model: [qsTr("Automatic"), qsTr("IPv4"), qsTr("IPv6")]
                currentIndex: App.ipVersion === 4 ? 1 : App.ipVersion === 6 ? 2 : 0
                onActivated: (i) => App.ipVersion = [0, 4, 6][i]
            }
        }
        SettingRow {
            title: qsTr("User agent")
            description: qsTr("How FluxDM introduces itself to web servers. Leave empty for the default. Downloads from the browser use the browser's.")
            FTextField {
                implicitWidth: 300
                placeholderText: App.defaultUserAgent
                Component.onCompleted: text = App.userAgent
                onEditingFinished: App.userAgent = text
            }
        }
        SettingRow {
            title: qsTr("Connections per server")
            description: qsTr("Across all downloads from the same site. Some servers block clients that open too many.")
            divider: false
            FSpinBox {
                from: 0; to: 64
                zeroText: qsTr("No limit")
                Component.onCompleted: value = App.maxConnectionsPerServer()
                onValueModified: App.setMaxConnectionsPerServer(value)
            }
        }
    }
}
