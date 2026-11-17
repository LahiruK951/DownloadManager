import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: panel
    property var download: null
    signal closeRequested()

    readonly property var d: download
    color: Theme.surface

    component InfoLabel: Text {
        color: Theme.textDim
        font.pixelSize: Theme.fontSmall + 0.5
        Layout.alignment: Qt.AlignTop
        Layout.preferredWidth: 130
        topPadding: 1
    }
    component InfoValue: TextEdit {
        Layout.fillWidth: true
        readOnly: true
        selectByMouse: true
        wrapMode: TextEdit.WrapAnywhere
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: "white"
        font.pixelSize: Theme.fontBody
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border }

        // ------------------------------------------------ header: tabs + quick actions
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            Layout.leftMargin: 10
            Layout.rightMargin: 8
            spacing: 4

            TabBar {
                id: tabs
                currentIndex: App.bottomPanelTab
                onCurrentIndexChanged: App.bottomPanelTab = currentIndex
                background: Item {}
                Layout.alignment: Qt.AlignBottom
                Repeater {
                    model: [qsTr("General"), qsTr("Files"), qsTr("Log")]
                    TabButton {
                        id: tab
                        required property string modelData
                        required property int index
                        readonly property bool current: tabs.currentIndex === index
                        width: implicitWidth + 20
                        height: 40
                        hoverEnabled: true
                        contentItem: Text {
                            text: tab.modelData
                            color: tab.current ? Theme.text : (tab.hovered ? Theme.text : Theme.textDim)
                            font.pixelSize: Theme.fontBody
                            font.weight: tab.current ? Font.DemiBold : Font.Normal
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Item {
                            Rectangle {
                                anchors.bottom: parent.bottom
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: parent.width - 16
                                height: 3
                                radius: 1.5
                                color: Theme.accent
                                visible: tab.current
                            }
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true }

            FButton {
                kind: "ghost"
                glyph: Theme.icons.open
                text: qsTr("Open")
                visible: panel.d !== null && panel.d.completed
                onClicked: App.openFile(panel.d.id)
            }
            FButton {
                kind: "ghost"
                glyph: Theme.icons.folder
                text: qsTr("Show in folder")
                visible: panel.d !== null
                onClicked: App.openFolder(panel.d.id)
            }
            FButton {
                kind: "ghost"
                glyph: Theme.icons.copy
                text: qsTr("Copy link")
                visible: panel.d !== null
                onClicked: App.copyLink(panel.d.id)
            }
            Rectangle { width: 1; height: 20; color: Theme.border; Layout.leftMargin: 4; Layout.rightMargin: 4 }
            IconButton { glyph: Theme.icons.close; glyphSize: 11; size: 30; tip: qsTr("Close panel"); onClicked: panel.closeRequested() }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.border; opacity: 0.6 }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            // ---------------------------------------------------- General
            ScrollView {
                id: generalScroll
                clip: true
                contentWidth: availableWidth
                ColumnLayout {
                    width: generalScroll.availableWidth
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        Layout.topMargin: 14
                        spacing: 6
                        visible: panel.d !== null && !panel.d.completed
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: qsTr("Progress"); color: Theme.textDim; font.pixelSize: Theme.fontSmall }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: panel.d && panel.d.progress >= 0 ? (panel.d.progress * 100).toFixed(1) + "%" : ""
                                color: Theme.text
                                font.pixelSize: Theme.fontSmall
                                font.weight: Font.DemiBold
                            }
                        }
                        SegmentBar {
                            Layout.fillWidth: true
                            segments: panel.d ? panel.d.segments : []
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        Layout.topMargin: panel.d && panel.d.completed ? 14 : 0
                        Layout.bottomMargin: 14
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 8
                        visible: panel.d !== null

                        InfoLabel { text: qsTr("Status") }
                        InfoValue {
                            text: !panel.d ? "" : panel.d.failed ? panel.d.stateText + " — " + panel.d.error
                                 : panel.d.seeding ? qsTr("Seeding") : panel.d.stateText
                            color: panel.d && panel.d.failed ? Theme.error : Theme.text
                        }
                        InfoLabel { text: qsTr("Address") }
                        InfoValue { text: panel.d ? panel.d.url : ""; color: Theme.accent }
                        InfoLabel { text: qsTr("Saved to") }
                        InfoValue { text: panel.d ? (panel.d.filePath || panel.d.destDir) : "" }
                        InfoLabel { text: qsTr("Size") }
                        InfoValue {
                            text: !panel.d ? "" : (panel.d.total >= 0 ? App.fmtSize(panel.d.total) : qsTr("Unknown"))
                                  + (panel.d.completed || panel.d.received === 0 ? "" : "  ·  " + App.fmtSize(panel.d.received) + " " + qsTr("downloaded"))
                        }
                        InfoLabel { text: qsTr("Speed"); visible: panel.d && panel.d.running }
                        InfoValue {
                            visible: panel.d && panel.d.running
                            text: panel.d ? App.fmtSpeed(panel.d.speed) + "  ·  " + App.fmtEta(panel.d.eta) + " " + qsTr("left")
                                            + "  ·  " + panel.d.connections + " " + qsTr("connections") : ""
                        }
                        InfoLabel { text: qsTr("Resume support") }
                        InfoValue {
                            text: !panel.d ? "" : panel.d.resumable === 1 ? qsTr("Yes")
                                  : panel.d.resumable === 0 ? qsTr("No — a restart begins from zero") : qsTr("Unknown")
                        }
                        InfoLabel { text: qsTr("Seeds / peers"); visible: panel.d && panel.d.isTorrent }
                        InfoValue { visible: panel.d && panel.d.isTorrent; text: panel.d ? panel.d.seeds + " / " + panel.d.peers : "" }
                        InfoLabel { text: qsTr("Uploaded"); visible: panel.d && panel.d.isTorrent }
                        InfoValue {
                            visible: panel.d && panel.d.isTorrent
                            text: panel.d ? App.fmtSize(panel.d.uploaded) + "  ·  " + qsTr("ratio") + " " + panel.d.ratio.toFixed(2)
                                            + "  ·  ↑ " + App.fmtSpeed(panel.d.uploadSpeed) : ""
                        }
                        InfoLabel { text: qsTr("Schedule"); visible: panel.d && panel.d.scheduled }
                        InfoValue { visible: panel.d && panel.d.scheduled; text: panel.d ? panel.d.scheduleText : "" }
                        InfoLabel { text: qsTr("Added") }
                        InfoValue { text: panel.d ? App.fmtDate(panel.d.created) : "" }
                        InfoLabel { text: qsTr("Completed"); visible: panel.d && panel.d.completed }
                        InfoValue { visible: panel.d && panel.d.completed; text: panel.d ? App.fmtDate(panel.d.finishedAt) : "" }
                    }
                }
            }

            // ---------------------------------------------------- Files
            ListView {
                id: filesView
                clip: true
                topMargin: 6
                bottomMargin: 6
                ScrollBar.vertical: ScrollBar {}
                readonly property bool multi: panel.d !== null && panel.d.files.length > 0
                model: !panel.d ? []
                       : multi ? panel.d.files
                       : [{ name: panel.d.title, size: panel.d.total, done: panel.d.received, wanted: true, index: -1 }]
                delegate: Item {
                    id: fileRow
                    required property var modelData
                    width: filesView.width
                    height: 38
                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        radius: 6
                        color: fileHover.hovered ? Theme.hover : "transparent"
                    }
                    HoverHandler { id: fileHover }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        spacing: 12
                        FCheckBox {
                            visible: filesView.multi
                            padding: 0
                            checked: fileRow.modelData.wanted
                            enabled: panel.d && !panel.d.completed
                            onToggled: App.setFileWanted(panel.d.id, fileRow.modelData.index, checked)
                        }
                        Text {
                            Layout.fillWidth: true
                            text: fileRow.modelData.name
                            color: fileRow.modelData.wanted ? Theme.text : Theme.textDisabled
                            elide: Text.ElideMiddle
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            text: fileRow.modelData.size >= 0 ? App.fmtSize(fileRow.modelData.size) : ""
                            color: Theme.textDim
                            font.pixelSize: Theme.fontSmall + 0.5
                            Layout.preferredWidth: 80
                            horizontalAlignment: Text.AlignRight
                        }
                        ProgressTrack {
                            Layout.preferredWidth: 160
                            value: fileRow.modelData.size > 0 ? fileRow.modelData.done / fileRow.modelData.size
                                   : (panel.d && panel.d.completed ? 1 : -1)
                            active: panel.d ? panel.d.running : false
                            failed: panel.d ? panel.d.failed : false
                        }
                        Text {
                            Layout.preferredWidth: 40
                            horizontalAlignment: Text.AlignRight
                            text: fileRow.modelData.size > 0 ? Math.floor(100 * fileRow.modelData.done / fileRow.modelData.size) + "%"
                                  : (panel.d && panel.d.completed ? "100%" : "")
                            color: Theme.textDim
                            font.pixelSize: Theme.fontSmall + 0.5
                        }
                    }
                }
            }

            // ---------------------------------------------------- Log
            ListView {
                id: logView
                clip: true
                model: panel.d ? panel.d.log : []
                ScrollBar.vertical: ScrollBar {}
                topMargin: 10
                bottomMargin: 10
                spacing: 2
                onCountChanged: positionViewAtEnd()
                delegate: TextEdit {
                    required property string modelData
                    width: logView.width
                    leftPadding: 20
                    rightPadding: 20
                    text: modelData
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.WrapAnywhere
                    color: modelData.indexOf("Error") >= 0 ? Theme.error : Theme.text
                    selectionColor: Theme.accent
                    selectedTextColor: "white"
                    font.family: "Consolas"
                    font.pixelSize: 12
                }
            }
        }
    }
}
