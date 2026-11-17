import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    implicitHeight: 36
    color: Theme.surface

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 10
        spacing: 6

        Text { text: Theme.icons.down; font.family: Theme.iconFont; font.pixelSize: 12; color: Theme.accent }
        Text { text: App.fmtSpeed(App.totalSpeed); color: Theme.text; font.pixelSize: 12; Layout.preferredWidth: 90 }
        Text { text: Theme.icons.up; font.family: Theme.iconFont; font.pixelSize: 12; color: Theme.success }
        Text { text: App.fmtSpeed(App.totalUpload); color: Theme.text; font.pixelSize: 12; Layout.preferredWidth: 80 }
        Text {
            text: App.activeCount > 0 ? App.activeCount + " " + qsTr("active") : ""
            color: Theme.textDim
            font.pixelSize: 12
        }

        Item { Layout.fillWidth: true }

        Text {
            visible: App.whenDone !== 0
            text: Theme.icons.power
            font.family: Theme.iconFont
            font.pixelSize: 12
            color: Theme.warning
        }
        Text {
            visible: App.whenDone !== 0
            text: ["", qsTr("Sleep"), qsTr("Hibernate"), qsTr("Shut down"), qsTr("Exit")][App.whenDone] + " " + qsTr("when done")
            font.pixelSize: 12
            color: Theme.warning
            Layout.rightMargin: 10
        }
        Text {
            visible: App.trafficScheduled
            text: Theme.icons.clock
            font.family: Theme.iconFont
            font.pixelSize: 13
            color: Theme.accent
            HoverHandler { id: clockHover }
            ToolTip.visible: clockHover.hovered
            ToolTip.text: qsTr("Traffic mode is set by your schedule. Pick a mode to override it until the scheduled period ends.")
        }
        Text { text: qsTr("Traffic mode"); color: Theme.textDim; font.pixelSize: 12; Layout.rightMargin: 4 }

        Rectangle {
            implicitWidth: modes.implicitWidth + 4
            implicitHeight: 26
            radius: 13
            color: Theme.surfaceAlt

            Row {
                id: modes
                anchors.centerIn: parent
                spacing: 0
                Repeater {
                    model: [qsTr("Snail"), qsTr("Medium"), qsTr("High")]
                    delegate: AbstractButton {
                        id: modeBtn
                        required property string modelData
                        required property int index
                        readonly property bool current: App.trafficMode === index
                        implicitWidth: label.implicitWidth + 22
                        implicitHeight: 22
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus
                        onClicked: App.trafficMode = index
                        background: Rectangle {
                            radius: 11
                            color: modeBtn.current ? Theme.accent : (modeBtn.hovered ? Theme.hover : "transparent")
                        }
                        contentItem: Text {
                            id: label
                            text: modeBtn.modelData
                            color: modeBtn.current ? "white" : Theme.text
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: App.tumDescription(index)
                        ToolTip.delay: 400
                    }
                }
            }
        }
    }
}
