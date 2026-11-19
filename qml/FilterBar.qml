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
        { key: "status", label: qsTr("Status"), asc: qsTr("Active first"), desc: qsTr("Finished first") },
        { key: "queue", label: qsTr("Queue order"), asc: qsTr("Queue order"), desc: qsTr("Queue order, reversed") }
    ]
    signal newTagRequested()
    signal editTagRequested(var tag)

    readonly property var currentSort: sortKeys.find(k => k.key === App.sortKey) || sortKeys[0]
    function openSortMenu() { sortMenu.popup(sortBtn, sortBtn.width - sortMenu.width, sortBtn.height + 4) }

    Rectangle { // fade at the right edge while more chips are hidden there
        z: 1
        visible: chipScroll.contentX < chipScroll.contentWidth - chipScroll.width - 1
        anchors.right: chipScroll.right
        anchors.top: chipScroll.top
        width: 36
        height: 28
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0) }
            GradientStop { position: 1; color: Theme.bg }
        }
    }

    // One line of chips that scrolls sideways (wheel or drag) when tags don't fit.
    Flickable {
        id: chipScroll
        anchors.fill: parent
        anchors.rightMargin: sortBtn.width + 16
        anchors.leftMargin: 12
        anchors.topMargin: 9
        contentWidth: chipRow.width
        contentHeight: chipRow.height
        flickableDirection: Flickable.HorizontalFlick
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        WheelHandler { // vertical wheel scrolls the chips sideways
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => {
                const dx = event.angleDelta.y !== 0 ? -event.angleDelta.y : -event.angleDelta.x
                chipScroll.contentX = Math.max(0, Math.min(chipScroll.contentWidth - chipScroll.width, chipScroll.contentX + dx / 2))
            }
        }

    Row {
        id: chipRow
        spacing: 6

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
        Repeater { // user tags
            model: App.tags
            delegate: Chip {
                required property var modelData
                readonly property string key: "tag:" + modelData.id
                label: modelData.name
                dot: modelData.color
                count: App.counts[key] || 0
                active: App.filter === key
                onClicked: App.filter = key
                onRightClicked: { tagMenu.tag = modelData; tagMenu.popup() }
                ToolTip.visible: hovered && modelData.extensions !== ""
                ToolTip.delay: 600
                ToolTip.text: qsTr("Added automatically to: %1").arg(modelData.extensions)
            }
        }
        Chip {
            label: "+"
            ToolTip.visible: hovered
            ToolTip.delay: 400
            ToolTip.text: qsTr("New tag")
            onClicked: root.newTagRequested()
        }
    }
    }

    FMenu {
        id: tagMenu
        property var tag: null
        implicitWidth: 180
        FMenuItem { glyph: String.fromCharCode(0xE70F); text: qsTr("Edit tag…"); onTriggered: root.editTagRequested(tagMenu.tag) }
        FMenuItem { glyph: Theme.icons.del; danger: true; text: qsTr("Delete tag"); onTriggered: App.removeTag(tagMenu.tag.id) }
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
        FMenuItem { text: root.sortKeys[4].label; checkable: true; checked: App.sortKey === "queue"; onTriggered: App.setSort("queue", false) }
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
