import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: dlg
    title: qsTr("Settings")
    modal: true
    anchors.centerIn: parent
    width: Math.min(720, parent.width - 40)
    height: Math.min(680, parent.height - 40)
    padding: 20

    property var tum: []
    property var rules: []
    readonly property var modeNames: [qsTr("Low (snail)"), qsTr("Medium"), qsTr("High")]

    onAboutToShow: {
        tum = [App.tumSettings(0), App.tumSettings(1), App.tumSettings(2)]
        rules = App.trafficRules()
        themeBox.currentIndex = ["system", "light", "dark"].indexOf(App.theme)
        folderField.text = App.defaultFolder
        hideBox.checked = App.closeButtonHidesApp
        clipBox.checked = App.monitorClipboard
        notifyBox.checked = App.notifyOnComplete
        upField.value = App.uploadLimitKBps()
        ratioField.value = Math.round(App.seedRatio() * 10)
        assocBox.checked = App.torrentAssociated()
    }

    function addRule() {
        const r = rules.slice()
        r.push({ enabled: true, days: 0x1F, from: 9 * 60, to: 18 * 60, mode: 0 }) // weekdays office hours → snail
        rules = r
    }
    function removeRule(i) {
        const r = rules.slice()
        r.splice(i, 1)
        rules = r
    }

    function save() {
        App.theme = ["system", "light", "dark"][themeBox.currentIndex]
        App.defaultFolder = folderField.text
        App.closeButtonHidesApp = hideBox.checked
        App.monitorClipboard = clipBox.checked
        App.notifyOnComplete = notifyBox.checked
        for (let m = 0; m < 3; ++m)
            App.setTumSettings(m, tum[m].maxDownloads, tum[m].maxConnections, tum[m].speedLimit)
        App.setTrafficRules(rules)
        App.setUploadLimitKBps(upField.value)
        App.setSeedRatio(ratioField.value / 10)
        if (assocBox.checked !== App.torrentAssociated())
            App.setTorrentAssociated(assocBox.checked)
        close()
    }

    background: Rectangle { color: Theme.surface; radius: 10; border.color: Theme.border }
    header: Label { text: dlg.title; color: Theme.text; font.pixelSize: 17; padding: 20; bottomPadding: 0 }

    component SectionTitle: Label {
        color: Theme.text
        font.pixelSize: 14
        font.bold: true
        Layout.topMargin: 14
    }
    component Hint: Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textDim
        font.pixelSize: 12
    }

    contentItem: ScrollView {
        id: scroller
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: scroller.availableWidth - 12
            spacing: 8

            SectionTitle { text: qsTr("General"); Layout.topMargin: 0 }
            RowLayout {
                Label { text: qsTr("Theme"); color: Theme.textDim; Layout.preferredWidth: 150 }
                ComboBox { id: themeBox; model: [qsTr("System"), qsTr("Light"), qsTr("Dark")]; Layout.preferredWidth: 180 }
            }
            CheckBox { id: hideBox; text: qsTr("Closing the window keeps FluxDM running in the tray") }
            CheckBox { id: clipBox; text: qsTr("Offer to download links copied to the clipboard") }
            CheckBox { id: notifyBox; text: qsTr("Show a notification when a download completes") }

            SectionTitle { text: qsTr("Downloads") }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Default folder"); color: Theme.textDim; Layout.preferredWidth: 150 }
                TextField { id: folderField; Layout.fillWidth: true; color: Theme.text; selectByMouse: true }
                Button { text: qsTr("Browse…"); onClicked: folderDlg.open() }
            }

            SectionTitle { text: qsTr("Traffic modes") }
            Hint { text: qsTr("Switch modes from the status bar. Speed limit is in KB/s; 0 means unlimited. Low (snail) mode also throttles torrent uploads.") }
            GridLayout {
                columns: 4
                columnSpacing: 10
                rowSpacing: 6

                Item { implicitWidth: 1 }
                Repeater {
                    model: dlg.modeNames
                    Label { required property string modelData; text: modelData; color: Theme.text; Layout.alignment: Qt.AlignHCenter }
                }

                Label { text: qsTr("Simultaneous downloads"); color: Theme.textDim }
                Repeater {
                    model: 3
                    SpinBox {
                        required property int index
                        from: 1; to: 50; editable: true
                        value: dlg.tum.length ? dlg.tum[index].maxDownloads : 1
                        onValueModified: dlg.tum[index].maxDownloads = value
                    }
                }
                Label { text: qsTr("Connections per download"); color: Theme.textDim }
                Repeater {
                    model: 3
                    SpinBox {
                        required property int index
                        from: 1; to: 32; editable: true
                        value: dlg.tum.length ? dlg.tum[index].maxConnections : 1
                        onValueModified: dlg.tum[index].maxConnections = value
                    }
                }
                Label { text: qsTr("Speed limit (KB/s)"); color: Theme.textDim }
                Repeater {
                    model: 3
                    SpinBox {
                        required property int index
                        from: 0; to: 1000000; stepSize: 64; editable: true
                        value: dlg.tum.length ? dlg.tum[index].speedLimit : 0
                        onValueModified: dlg.tum[index].speedLimit = value
                    }
                }
            }

            SectionTitle { text: qsTr("Traffic mode schedule") }
            Hint { text: qsTr("Switch modes automatically, e.g. snail mode during working hours. The first matching rule wins; outside all rules the mode you pick in the status bar is used. Picking a mode by hand overrides the active rule until its time window ends.") }
            Repeater {
                model: dlg.rules
                delegate: Rectangle {
                    id: ruleRow
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    implicitHeight: ruleLayout.implicitHeight + 16
                    radius: 8
                    color: Theme.surfaceAlt
                    RowLayout {
                        id: ruleLayout
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 8
                        CheckBox {
                            checked: ruleRow.modelData.enabled
                            onToggled: dlg.rules[ruleRow.index].enabled = checked
                        }
                        ScheduleEditor {
                            days: ruleRow.modelData.days
                            fromMin: ruleRow.modelData.from
                            toMin: ruleRow.modelData.to
                            onEdited: {
                                dlg.rules[ruleRow.index].days = days
                                dlg.rules[ruleRow.index].from = fromMin
                                dlg.rules[ruleRow.index].to = toMin
                            }
                        }
                        Label { text: "→"; color: Theme.textDim }
                        ComboBox {
                            model: dlg.modeNames
                            currentIndex: ruleRow.modelData.mode
                            Layout.preferredWidth: 130
                            onActivated: (i) => dlg.rules[ruleRow.index].mode = i
                        }
                        Item { Layout.fillWidth: true }
                        IconButton { glyph: Theme.icons.del; tip: qsTr("Remove rule"); onClicked: dlg.removeRule(ruleRow.index) }
                    }
                }
            }
            Button { text: qsTr("Add rule"); onClicked: dlg.addRule() }

            SectionTitle { text: qsTr("BitTorrent"); visible: App.torrentSupport }
            GridLayout {
                visible: App.torrentSupport
                columns: 2
                columnSpacing: 10
                rowSpacing: 6
                Label { text: qsTr("Upload limit (KB/s, 0 = unlimited)"); color: Theme.textDim }
                SpinBox { id: upField; from: 0; to: 1000000; stepSize: 16; editable: true }
                Label { text: qsTr("Stop seeding at ratio (0 = don't seed)"); color: Theme.textDim }
                SpinBox {
                    id: ratioField
                    from: 0; to: 1000; stepSize: 1; editable: true
                    textFromValue: (v) => (v / 10).toFixed(1)
                    valueFromText: (t) => Math.round(parseFloat(t) * 10)
                }
            }
            CheckBox { id: assocBox; visible: App.torrentSupport; text: qsTr("Open magnet links and .torrent files with FluxDM") }
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
            Button { text: qsTr("Save"); highlighted: true; onClicked: dlg.save() }
        }
    }

    FolderDialog {
        id: folderDlg
        currentFolder: App.toUrl(folderField.text)
        onAccepted: folderField.text = App.urlToLocal(selectedFolder)
    }
}
