import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    implicitHeight: 36
    function openTraffic() { trafficPopup.open() }
    color: Theme.surface

    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.border }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 10
        spacing: 6

        // Speeds: click for the speed graph and traffic totals.
        RowLayout {
            spacing: 6
            Text { text: Theme.icons.down; font.family: Theme.iconFont; font.pixelSize: 12; color: Theme.accent }
            Text { text: App.fmtSpeed(App.totalSpeed); color: Theme.text; font.pixelSize: 12; Layout.preferredWidth: 90 }
            Text { text: Theme.icons.up; font.family: Theme.iconFont; font.pixelSize: 12; color: Theme.success }
            Text { text: App.fmtSpeed(App.totalUpload); color: Theme.text; font.pixelSize: 12; Layout.preferredWidth: 80 }
            HoverHandler { id: speedHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: trafficPopup.visible ? trafficPopup.close() : trafficPopup.open() }
            ToolTip.visible: speedHover.hovered && !trafficPopup.visible
            ToolTip.text: qsTr("Speed graph and traffic totals")
            ToolTip.delay: 600
            TrafficPopup { id: trafficPopup; y: -height - 6 }
        }
        Text {
            text: App.activeCount > 0 ? App.activeCount + " " + qsTr("active") : ""
            color: Theme.textDim
            font.pixelSize: 12
        }
        Text {
            visible: App.selectionCount > 1
            text: "·  " + App.selectionCount + " " + qsTr("selected")
            color: Theme.accent
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

        FSegmented {
            itemHeight: 22
            model: [qsTr("Snail"), qsTr("Medium"), qsTr("High")]
            currentIndex: App.trafficMode
            tips: [App.tumDescription(0), App.tumDescription(1), App.tumDescription(2)]
            onActivated: (i) => App.trafficMode = i
        }
    }
}
