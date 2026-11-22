import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings > Browser: browser integration and extensions.
ColumnLayout {
    function load() { // when Settings opens
        browserToggle.checked = App.browserIntegrated()
        browserError.text = ""
    }
    spacing: 20
    SettingsCard {
        title: qsTr("Integration")
        Layout.fillWidth: true
        SettingRow {
            title: qsTr("Connect FluxDM to your browsers")
            description: qsTr("Registers the FluxDM helper for Chrome, Edge, Brave and Firefox (your Windows account only).")
            FToggle {
                id: browserToggle
                onToggled: {
                    browserError.text = App.setBrowserIntegrated(checked)
                    checked = App.browserIntegrated()
                }
            }
        }
        SettingRow {
            title: qsTr("Add browser downloads without asking")
            description: qsTr("Otherwise FluxDM shows the New download dialog first.")
            divider: false
            FToggle { checked: App.browserAddSilently; onToggled: App.browserAddSilently = checked }
        }
    }
    Rectangle {
        Layout.fillWidth: true
        visible: browserError.text !== ""
        implicitHeight: browserError.implicitHeight + 20
        radius: Theme.radius
        color: Theme.errorSoft
        Text {
            id: browserError
            anchors.fill: parent
            anchors.margins: 10
            color: Theme.error
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSmall + 0.5
        }
    }
    SettingsCard {
        title: qsTr("Install the extension: Chrome, Edge, Brave")
        description: qsTr("One-time setup in each browser:")
        Layout.fillWidth: true
        Repeater {
            model: [
                qsTr("Open chrome://extensions (Edge: edge://extensions, Brave: brave://extensions)."),
                qsTr("Turn on Developer mode."),
                qsTr("Click Load unpacked and choose the FluxDM extension folder below."),
                qsTr("Turn on “Connect FluxDM to your browsers” above.")
            ]
            delegate: RowLayout {
                required property string modelData
                required property int index
                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 12
                Rectangle {
                    implicitWidth: 22
                    implicitHeight: 22
                    radius: 11
                    color: Theme.accentSoft
                    Text { anchors.centerIn: parent; text: index + 1; color: Theme.accent; font.pixelSize: 11; font.weight: Font.DemiBold }
                }
                Text {
                    Layout.fillWidth: true
                    text: modelData
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }
            }
        }
        SettingRow {
            Layout.topMargin: 6
            title: qsTr("Extension folder")
            description: App.extensionDir()
            divider: false
            FButton { glyph: Theme.icons.folder; text: qsTr("Open folder"); onClicked: App.openExtensionDir() }
        }
    }
    SettingsCard {
        title: qsTr("Install the extension: Firefox")
        description: qsTr("Firefox 128 or newer:")
        Layout.fillWidth: true
        Repeater {
            model: [
                qsTr("Open about:debugging#/runtime/this-firefox."),
                qsTr("Click Load Temporary Add-on… and choose manifest.json in the Firefox extension folder below."),
                qsTr("Turn on “Connect FluxDM to your browsers” above.")
            ]
            delegate: RowLayout {
                required property string modelData
                required property int index
                Layout.fillWidth: true
                Layout.topMargin: 10
                spacing: 12
                Rectangle {
                    implicitWidth: 22
                    implicitHeight: 22
                    radius: 11
                    color: Theme.accentSoft
                    Text { anchors.centerIn: parent; text: index + 1; color: Theme.accent; font.pixelSize: 11; font.weight: Font.DemiBold }
                }
                Text {
                    Layout.fillWidth: true
                    text: modelData
                    color: Theme.text
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                }
            }
        }
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 10
            text: qsTr("Firefox removes temporary add-ons when it restarts. To keep it installed, the extension has to be signed by Mozilla (free, on addons.mozilla.org).")
            color: Theme.textDim
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSmall
        }
        SettingRow {
            Layout.topMargin: 6
            title: qsTr("Firefox extension folder")
            description: App.firefoxExtensionDir()
            divider: false
            FButton { glyph: Theme.icons.folder; text: qsTr("Open folder"); onClicked: App.openFirefoxExtensionDir() }
        }
    }
}
