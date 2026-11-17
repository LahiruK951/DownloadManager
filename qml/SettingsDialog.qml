import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

Dialog {
    id: dlg
    title: qsTr("Settings")
    modal: true
    anchors.centerIn: parent
    width: Math.min(640, parent.width - 40)
    height: Math.min(600, parent.height - 40)
    padding: 20

    property var tum: []

    onAboutToShow: {
        tum = [App.tumSettings(0), App.tumSettings(1), App.tumSettings(2)]
        themeBox.currentIndex = ["system", "light", "dark"].indexOf(App.theme)
        folderField.text = App.defaultFolder
        hideBox.checked = App.closeButtonHidesApp
        clipBox.checked = App.monitorClipboard
        notifyBox.checked = App.notifyOnComplete
    }

    function save() {
        App.theme = ["system", "light", "dark"][themeBox.currentIndex]
        App.defaultFolder = folderField.text
        App.closeButtonHidesApp = hideBox.checked
        App.monitorClipboard = clipBox.checked
        App.notifyOnComplete = notifyBox.checked
        for (let m = 0; m < 3; ++m)
            App.setTumSettings(m, tum[m].maxDownloads, tum[m].maxConnections, tum[m].speedLimit)
        close()
    }

    background: Rectangle { color: Theme.surface; radius: 10; border.color: Theme.border }
    header: Label { text: dlg.title; color: Theme.text; font.pixelSize: 17; padding: 20; bottomPadding: 0 }

    component SectionTitle: Label {
        color: Theme.text
        font.pixelSize: 14
        font.bold: true
        Layout.topMargin: 10
    }

    contentItem: ScrollView {
        clip: true
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: 8

            SectionTitle { text: qsTr("General"); Layout.topMargin: 0 }
            RowLayout {
                Label { text: qsTr("Theme"); color: Theme.textDim; Layout.preferredWidth: 140 }
                ComboBox { id: themeBox; model: [qsTr("System"), qsTr("Light"), qsTr("Dark")]; Layout.preferredWidth: 180 }
            }
            CheckBox { id: hideBox; text: qsTr("Closing the window keeps FluxDM running in the tray") }
            CheckBox { id: clipBox; text: qsTr("Offer to download links copied to the clipboard") }
            CheckBox { id: notifyBox; text: qsTr("Show a notification when a download completes") }

            SectionTitle { text: qsTr("Downloads") }
            RowLayout {
                Layout.fillWidth: true
                Label { text: qsTr("Default folder"); color: Theme.textDim; Layout.preferredWidth: 140 }
                TextField { id: folderField; Layout.fillWidth: true; color: Theme.text; selectByMouse: true }
                Button { text: qsTr("Browse…"); onClicked: folderDlg.open() }
            }

            SectionTitle { text: qsTr("Traffic modes") }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textDim
                text: qsTr("Switch modes from the status bar. Speed limit is in KB/s; 0 means unlimited.")
            }
            GridLayout {
                columns: 4
                columnSpacing: 10
                rowSpacing: 6

                Item { implicitWidth: 1 }
                Label { text: qsTr("Low"); color: Theme.text; Layout.alignment: Qt.AlignHCenter }
                Label { text: qsTr("Medium"); color: Theme.text; Layout.alignment: Qt.AlignHCenter }
                Label { text: qsTr("High"); color: Theme.text; Layout.alignment: Qt.AlignHCenter }

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
