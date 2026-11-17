import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root
    implicitHeight: 46
    color: Theme.bg

    readonly property var chips: [
        { key: "all", label: qsTr("All"), fixed: true },
        { key: "active", label: qsTr("Active"), fixed: true },
        { key: "completed", label: qsTr("Completed"), fixed: true },
        { key: "Video", label: qsTr("Video") },
        { key: "Music", label: qsTr("Music") },
        { key: "Archive", label: qsTr("Archives") },
        { key: "Docs", label: qsTr("Documents") },
        { key: "Programs", label: qsTr("Programs") },
        { key: "Pictures", label: qsTr("Pictures") },
        { key: "Other", label: qsTr("Other") }
    ]

    Flow {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.topMargin: 9
        spacing: 6
        clip: true

        Repeater {
            model: root.chips
            delegate: Chip {
                required property var modelData
                label: modelData.label
                count: App.counts[modelData.key] || 0
                active: App.filter === modelData.key
                visible: modelData.fixed === true || count > 0 || active
                onClicked: App.filter = modelData.key
            }
        }
    }
}
