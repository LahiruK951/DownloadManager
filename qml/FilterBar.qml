import QtQuick
import QtQuick.Controls
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

    // Sort: key + direction, remembered between runs.
    readonly property var sortKeys: [
        { key: "added", label: qsTr("Date added"), asc: qsTr("Oldest first"), desc: qsTr("Newest first") },
        { key: "name", label: qsTr("Name"), asc: qsTr("A to Z"), desc: qsTr("Z to A") },
        { key: "size", label: qsTr("Size"), asc: qsTr("Smallest first"), desc: qsTr("Largest first") },
        { key: "status", label: qsTr("Status"), asc: qsTr("Active first"), desc: qsTr("Finished first") }
    ]
    readonly property var currentSort: sortKeys.find(k => k.key === App.sortKey) || sortKeys[0]
    function openSortMenu() { sortMenu.popup(sortBtn, sortBtn.width - sortMenu.width, sortBtn.height + 4) }

    Flow {
        anchors.fill: parent
        anchors.rightMargin: sortBtn.width + 16
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

    Chip { // "Sort: Newest first ▾"
        id: sortBtn
        anchors.right: parent.right
        anchors.rightMargin: 12
        anchors.top: parent.top
        anchors.topMargin: 9
        label: (App.sortDescending ? root.currentSort.desc : root.currentSort.asc) + "  ▾"
        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: qsTr("Sort by %1").arg(root.currentSort.label.toLowerCase())
        onClicked: root.openSortMenu()
    }

    FMenu {
        id: sortMenu
        implicitWidth: 220
        // a new key starts in its natural direction: newest, A to Z, largest, active first
        FMenuItem { text: root.sortKeys[0].label; checkable: true; checked: App.sortKey === "added"; onTriggered: App.setSort("added", true) }
        FMenuItem { text: root.sortKeys[1].label; checkable: true; checked: App.sortKey === "name"; onTriggered: App.setSort("name", false) }
        FMenuItem { text: root.sortKeys[2].label; checkable: true; checked: App.sortKey === "size"; onTriggered: App.setSort("size", true) }
        FMenuItem { text: root.sortKeys[3].label; checkable: true; checked: App.sortKey === "status"; onTriggered: App.setSort("status", false) }
        FMenuSeparator {}
        FMenuItem {
            text: root.currentSort.asc
            checkable: true
            checked: !App.sortDescending
            onTriggered: App.setSort(App.sortKey, false)
        }
        FMenuItem {
            text: root.currentSort.desc
            checkable: true
            checked: App.sortDescending
            onTriggered: App.setSort(App.sortKey, true)
        }
    }
}
