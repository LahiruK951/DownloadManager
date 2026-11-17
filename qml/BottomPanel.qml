import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    property var download: null
    signal closeRequested()

    color: Theme.surface

    component InfoLabel: Text {
        color: Theme.textDim
        font.pixelSize: 13
        Layout.alignment: Qt.AlignTop
    }
    component InfoValue: TextEdit {
        Layout.fillWidth: true
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.WrapAnywhere
        color: Theme.text
        selectionColor: Theme.accent
        font.pixelSize: 13
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 38
            Layout.leftMargin: 8
            Layout.rightMargin: 6
            spacing: 0

            TabBar {
                id: tabs
                currentIndex: App.bottomPanelTab
                onCurrentIndexChanged: App.bottomPanelTab = currentIndex
                background: Item {}
                Repeater {
                    model: [qsTr("General"), qsTr("Files"), qsTr("Log")]
                    TabButton {
                        required property string modelData
                        required property int index
                        width: implicitWidth + 16
                        contentItem: Text {
                            text: modelData
                            color: tabs.currentIndex === index ? Theme.accent : Theme.textDim
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            color: "transparent"
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 2
                                color: Theme.accent
                                visible: tabs.currentIndex === index
                            }
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            IconButton { glyph: Theme.icons.close; glyphSize: 11; size: 28; tip: qsTr("Close panel"); onClicked: panel.closeRequested() }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ---------------------------------------------------- General
            ScrollView {
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    spacing: 10

                    SegmentBar {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.topMargin: 12
                        segments: panel.download ? panel.download.segments : []
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        Layout.bottomMargin: 12
                        columns: 2
                        columnSpacing: 18
                        rowSpacing: 6
                        visible: panel.download !== null

                        InfoLabel { text: qsTr("Status") }
                        InfoValue {
                            text: !panel.download ? "" : panel.download.failed
                                  ? panel.download.stateText + ": " + panel.download.error
                                  : panel.download.stateText
                        }
                        InfoLabel { text: qsTr("Address") }
                        InfoValue { text: panel.download ? panel.download.url : "" }
                        InfoLabel { text: qsTr("Saved to") }
                        InfoValue { text: panel.download ? (panel.download.filePath || panel.download.destDir) : "" }
                        InfoLabel { text: qsTr("Size") }
                        InfoValue { text: panel.download ? (panel.download.total >= 0 ? App.fmtSize(panel.download.total) : qsTr("Unknown")) : "" }
                        InfoLabel { text: qsTr("Downloaded") }
                        InfoValue {
                            text: !panel.download ? "" : App.fmtSize(panel.download.received)
                                  + (panel.download.progress >= 0 ? "  (" + (panel.download.progress * 100).toFixed(1) + "%)" : "")
                        }
                        InfoLabel { text: qsTr("Speed"); visible: panel.download && panel.download.running }
                        InfoValue {
                            visible: panel.download && panel.download.running
                            text: panel.download ? App.fmtSpeed(panel.download.speed) + "  ·  " + App.fmtEta(panel.download.eta) + " " + qsTr("left") : ""
                        }
                        InfoLabel { text: qsTr("Connections"); visible: panel.download && panel.download.running }
                        InfoValue { visible: panel.download && panel.download.running; text: panel.download ? String(panel.download.connections) : "" }
                        InfoLabel { text: qsTr("Resume support") }
                        InfoValue {
                            text: !panel.download ? "" : panel.download.resumable === 1 ? qsTr("Yes")
                                  : panel.download.resumable === 0 ? qsTr("No") : qsTr("Unknown")
                        }
                        InfoLabel { text: qsTr("Seeds / peers"); visible: panel.download && panel.download.isTorrent }
                        InfoValue {
                            visible: panel.download && panel.download.isTorrent
                            text: panel.download ? panel.download.seeds + " / " + panel.download.peers : ""
                        }
                        InfoLabel { text: qsTr("Uploaded"); visible: panel.download && panel.download.isTorrent }
                        InfoValue {
                            visible: panel.download && panel.download.isTorrent
                            text: panel.download ? App.fmtSize(panel.download.uploaded) + "  ·  " + qsTr("ratio") + " "
                                  + panel.download.ratio.toFixed(2) + "  ·  ↑ " + App.fmtSpeed(panel.download.uploadSpeed) : ""
                        }
                        InfoLabel { text: qsTr("Schedule"); visible: panel.download && panel.download.scheduled }
                        InfoValue { visible: panel.download && panel.download.scheduled; text: panel.download ? panel.download.scheduleText : "" }
                        InfoLabel { text: qsTr("Added") }
                        InfoValue { text: panel.download ? App.fmtDate(panel.download.created) : "" }
                        InfoLabel { text: qsTr("Completed"); visible: panel.download && panel.download.completed }
                        InfoValue { visible: panel.download && panel.download.completed; text: panel.download ? App.fmtDate(panel.download.finishedAt) : "" }
                    }
                }
            }

            // ---------------------------------------------------- Files
            ListView {
                id: filesView
                clip: true
                topMargin: 8
                ScrollBar.vertical: ScrollBar {}
                readonly property bool multi: panel.download !== null && panel.download.files.length > 0
                model: !panel.download ? []
                       : multi ? panel.download.files
                       : [{ name: panel.download.title, size: panel.download.total, done: panel.download.received, wanted: true, index: -1 }]
                delegate: RowLayout {
                    id: fileRow
                    required property var modelData
                    width: filesView.width - 32
                    x: 16
                    height: 30
                    spacing: 10
                    CheckBox {
                        visible: filesView.multi
                        checked: fileRow.modelData.wanted
                        enabled: panel.download && !panel.download.completed
                        onToggled: App.setFileWanted(panel.download.id, fileRow.modelData.index, checked)
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Download this file")
                    }
                    Text {
                        Layout.fillWidth: true
                        text: fileRow.modelData.name
                        color: fileRow.modelData.wanted ? Theme.text : Theme.textDisabled
                        elide: Text.ElideMiddle
                        font.pixelSize: 13
                    }
                    Text {
                        text: fileRow.modelData.size >= 0 ? App.fmtSize(fileRow.modelData.size) : ""
                        color: Theme.textDim
                        font.pixelSize: 13
                        Layout.preferredWidth: 80
                        horizontalAlignment: Text.AlignRight
                    }
                    ProgressTrack {
                        Layout.preferredWidth: 140
                        value: fileRow.modelData.size > 0 ? fileRow.modelData.done / fileRow.modelData.size
                               : (panel.download && panel.download.completed ? 1 : -1)
                        active: panel.download ? panel.download.running : false
                        failed: panel.download ? panel.download.failed : false
                    }
                    Text {
                        Layout.preferredWidth: 44
                        horizontalAlignment: Text.AlignRight
                        text: fileRow.modelData.size > 0 ? Math.floor(100 * fileRow.modelData.done / fileRow.modelData.size) + "%" : ""
                        color: Theme.textDim
                        font.pixelSize: 13
                    }
                }
            }
            // ---------------------------------------------------- Log
            ListView {
                id: logView
                clip: true
                model: panel.download ? panel.download.log : []
                ScrollBar.vertical: ScrollBar {}
                topMargin: 8
                bottomMargin: 8
                onCountChanged: positionViewAtEnd()
                delegate: TextEdit {
                    required property string modelData
                    width: logView.width - 32
                    x: 16
                    text: modelData
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: Theme.text
                    selectionColor: Theme.accent
                    font.family: "Consolas"
                    font.pixelSize: 12
                }
            }
        }
    }
}
