import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Per-download speed limit (FDM: "Speed limit" in the download's context menu).
FDialog {
    id: dlg
    property var target: null
    title: qsTr("Speed limit")
    subtitle: target ? target.title : ""
    width: Math.min(460, parent.width - 48)

    function openFor(d) {
        target = d
        limitBox.checked = d.speedLimit > 0
        valueField.value = d.speedLimit > 0 ? d.speedLimit : 1024
        open()
    }

    contentItem: ColumnLayout {
        spacing: 12
        FSwitch {
            id: limitBox
            Layout.fillWidth: true
            text: qsTr("Limit this download's speed")
            description: qsTr("Applies on top of the traffic mode's overall limit.")
        }
        RowLayout {
            enabled: limitBox.checked
            opacity: enabled ? 1 : 0.5
            spacing: 10
            FSpinBox {
                id: valueField
                implicitWidth: 150
                from: 16; to: 1000000; stepSize: 64
            }
            Text { text: qsTr("KB/s"); color: Theme.textDim; font.pixelSize: Theme.fontBody }
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            enabled: limitBox.checked
            opacity: enabled ? 1 : 0.5
            Repeater {
                model: [128, 512, 1024, 2048, 5120, 10240]
                delegate: Chip {
                    required property int modelData
                    label: modelData >= 1024 ? (modelData / 1024) + " MB/s" : modelData + " KB/s"
                    active: valueField.value === modelData
                    onClicked: valueField.value = modelData
                }
            }
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            text: qsTr("Apply")
            onClicked: {
                App.setDownloadSpeedLimit(dlg.target.id, limitBox.checked ? valueField.value : 0)
                dlg.close()
            }
        }
    ]
}
