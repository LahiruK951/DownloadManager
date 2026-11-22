import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings > General: appearance, behaviour, updates.
ColumnLayout {
    spacing: 20
    SettingsCard {
        title: qsTr("Appearance")
        Layout.fillWidth: true
        SettingRow {
            title: qsTr("Theme")
            FSegmented {
                model: [qsTr("System"), qsTr("Light"), qsTr("Dark")]
                currentIndex: ["system", "light", "dark"].indexOf(App.theme)
                onActivated: (i) => App.theme = ["system", "light", "dark"][i]
            }
        }
        SettingRow {
            title: qsTr("Compact list")
            description: qsTr("Show more downloads by hiding progress bars and details.")
            FToggle { checked: App.compactView; onToggled: App.compactView = checked }
        }
        SettingRow {
            id: langRow
            title: qsTr("Language")
            description: App.language !== App.languageInEffect ? qsTr("Takes effect when FluxDM restarts.")
                         : qsTr("Translations other than English were made by machine and may read awkwardly. Corrections are welcome.")
            RowLayout {
                spacing: 8
                FButton {
                    visible: App.language !== App.languageInEffect
                    kind: "ghost"
                    text: qsTr("Restart now")
                    onClicked: App.restartApp()
                }
                FComboBox {
                    Layout.preferredWidth: 220
                    model: App.languages.map(l => l.name)
                    currentIndex: Math.max(0, App.languages.findIndex(l => l.code === App.language))
                    onActivated: (i) => App.language = App.languages[i].code
                }
            }
        }
        SettingRow {
            id: zoomRow
            readonly property var levels: [80, 90, 100, 110, 125, 150, 175, 200]
            title: qsTr("Zoom")
            description: App.zoom !== App.zoomInEffect ? qsTr("Takes effect when FluxDM restarts.")
                                                       : qsTr("Size of text and controls.")
            divider: false
            RowLayout {
                spacing: 8
                FButton {
                    visible: App.zoom !== App.zoomInEffect
                    kind: "ghost"
                    text: qsTr("Restart now")
                    onClicked: App.restartApp()
                }
                FComboBox {
                    Layout.preferredWidth: 110
                    model: zoomRow.levels.map(z => z + " %")
                    currentIndex: Math.max(0, zoomRow.levels.indexOf(App.zoom))
                    onActivated: (i) => App.zoom = zoomRow.levels[i]
                }
            }
        }
    }
    SettingsCard {
        title: qsTr("Behavior")
        Layout.fillWidth: true
        SettingRow {
            title: qsTr("Start with Windows")
            description: qsTr("Starts in the notification area when you sign in, so scheduled and queued downloads continue.")
            FToggle { checked: App.startWithWindows; onToggled: App.startWithWindows = checked }
        }
        SettingRow {
            title: qsTr("Keep running in the notification area")
            description: qsTr("Closing the window hides FluxDM; downloads continue.")
            FToggle { checked: App.closeButtonHidesApp; onToggled: App.closeButtonHidesApp = checked }
        }
        SettingRow {
            title: qsTr("Watch the clipboard")
            description: qsTr("Offer to download when you copy a link to a file.")
            FToggle { checked: App.monitorClipboard; onToggled: App.monitorClipboard = checked }
        }
        SettingRow {
            title: qsTr("Keep the computer awake while downloading")
            description: qsTr("Windows won't go to sleep until downloads finish. The screen can still turn off.")
            FToggle { checked: App.preventSleep; onToggled: App.preventSleep = checked }
        }
        SettingRow {
            title: qsTr("Notify when a download completes")
            FToggle { checked: App.notifyOnComplete; onToggled: App.notifyOnComplete = checked }
        }
        SettingRow {
            title: qsTr("Play a sound when a download completes")
            divider: false
            FToggle { checked: App.soundOnComplete; onToggled: App.soundOnComplete = checked }
        }
    }
    SettingsCard {
        title: qsTr("Updates")
        Layout.fillWidth: true
        SettingRow {
            title: qsTr("FluxDM %1").arg(App.version)
            description: App.updateStatus !== "" ? App.updateStatus
                         : App.updateUrl === "" ? qsTr("No update address is set, so FluxDM doesn't look for updates.")
                         : qsTr("Looks for a new version once a day.")
            RowLayout {
                spacing: 8
                FButton {
                    visible: App.updateState === 2
                    kind: "primary"
                    text: qsTr("Download")
                    onClicked: App.downloadUpdate()
                }
                FButton {
                    visible: App.updateState === 4
                    kind: "primary"
                    text: qsTr("Install and restart")
                    onClicked: App.installUpdate()
                }
                FButton {
                    text: qsTr("Check now")
                    enabled: App.updateUrl !== "" && App.updateState !== 1 && App.updateState !== 3
                    onClicked: App.checkForUpdates()
                }
            }
        }
        SettingRow {
            title: qsTr("Check automatically")
            FToggle { checked: App.autoCheckUpdates; onToggled: App.autoCheckUpdates = checked }
        }
        SettingRow {
            title: qsTr("Update address")
            description: qsTr("Where the release information (latest.json) is published. HTTPS only.")
            divider: false
            FTextField {
                implicitWidth: 320
                text: App.updateUrl
                placeholderText: "https://…/latest.json"
                onEditingFinished: App.updateUrl = text
            }
        }
    }
}
