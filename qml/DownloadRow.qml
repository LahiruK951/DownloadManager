import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: row
    required property var download
    required property int index
    signal requestDelete(var download)
    signal requestSchedule(var download)
    signal requestDeleteSelection()
    signal requestSpeedLimit(var download)
    signal requestMove()
    signal requestRename(var download)

    readonly property var d: download
    readonly property bool isSelected: (App.selectionRev, App.isSelected(download.id))
    readonly property bool isCurrent: App.selected === download
    readonly property bool showActions: hoverH.hovered || isSelected
    readonly property bool compact: App.compactView
    readonly property bool hasProgress: !d.completed && (d.running || d.received > 0)

    height: compact ? 42 : 68
    color: isSelected ? Theme.selection : (hoverH.hovered ? Theme.hover : "transparent")

    HoverHandler { id: hoverH }

    Rectangle {
        visible: row.isCurrent && App.selectionCount > 1
        width: 3
        height: parent.height - 16
        radius: 2
        x: 4
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.accent
    }

    // ------------------------------------------------------------ text helpers
    function sizeLine() {
        if (d.total > 0)
            return App.fmtSize(d.received) + " " + qsTr("of") + " " + App.fmtSize(d.total)
        return d.received > 0 ? App.fmtSize(d.received) : ""
    }
    function metaLine() {
        if (d.completed && d.seeding)
            return qsTr("Seeding") + " · ↑ " + App.fmtSpeed(d.uploadSpeed) + " · " + qsTr("ratio") + " " + d.ratio.toFixed(2)
                   + " · " + d.peers + " " + qsTr("peers")
        if (d.completed && d.fileMissing) return qsTr("The file was moved or deleted")
        if (d.completed) return App.fmtDate(d.finishedAt)
        if (d.failed) return d.error + (d.retryIn >= 0 ? "  ·  " + qsTr("retrying in %1").arg(App.fmtEta(d.retryIn)) : "")
        if (d.waitingForSchedule) return d.scheduleText
        if (d.starting) return d.isTorrent ? qsTr("Fetching torrent information…") : qsTr("Connecting…")
        let parts = []
        const s = sizeLine()
        if (s !== "") parts.push(s)
        if (d.running) {
            if (d.isTorrent) parts.push(d.seeds + " " + qsTr("seeds") + ", " + d.peers + " " + qsTr("peers"))
            else if (d.connections > 0) parts.push(d.connections + " " + (d.connections === 1 ? qsTr("connection") : qsTr("connections")))
        }
        if (d.scheduled && !d.waitingForSchedule) parts.push(d.scheduleText)
        if (d.speedLimit > 0) parts.push(qsTr("max %1").arg(App.fmtSpeed(d.speedLimit * 1024)))
        return parts.join("  ·  ")
    }
    readonly property var pill: {
        if (d.converting) return { text: qsTr("Converting…"), glyph: Theme.icons.refresh, fg: Theme.accent, bg: Theme.accentSoft }
        if (d.moving) return { text: qsTr("Moving…"), glyph: Theme.icons.folder, fg: Theme.accent, bg: Theme.accentSoft }
        if (d.completed && d.seeding) return { text: qsTr("Seeding"), glyph: Theme.icons.upload, fg: Theme.accent, bg: Theme.accentSoft }
        if (d.completed && d.fileMissing) return { text: qsTr("File missing"), glyph: Theme.icons.warn, fg: Theme.warning, bg: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.15) }
        if (d.completed) return { text: qsTr("Done"), glyph: Theme.icons.check, fg: Theme.success, bg: Qt.rgba(Theme.success.r, Theme.success.g, Theme.success.b, 0.14) }
        if (d.failed) return { text: qsTr("Error"), glyph: Theme.icons.warn, fg: Theme.error, bg: Theme.errorSoft }
        if (d.paused) return { text: qsTr("Paused"), glyph: Theme.icons.pause, fg: Theme.textDim, bg: Theme.surfaceAlt }
        if (d.waitingForSchedule) return { text: qsTr("Scheduled"), glyph: Theme.icons.clock, fg: Theme.accent, bg: Theme.accentSoft }
        if (d.queued) return { text: qsTr("Queued"), glyph: "", fg: Theme.textDim, bg: Theme.surfaceAlt }
        return null
    }

    // ------------------------------------------------------------ input
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton) {
                if (!row.isSelected)
                    App.select(row.d.id)
                if (App.selectionCount > 1) multiCtx.popup()
                else ctx.popup()
                return
            }
            if (mouse.modifiers & Qt.ControlModifier) {
                App.toggleSelect(row.d.id)
                return
            }
            if (mouse.modifiers & Qt.ShiftModifier) {
                App.selectRange(row.d.id)
                return
            }
            if (row.isCurrent && App.selectionCount === 1)
                App.bottomPanelOpen = !App.bottomPanelOpen
            else {
                App.select(row.d.id)
                App.bottomPanelOpen = true
            }
        }
        onDoubleClicked: {
            if (row.d.completed) App.openFile(row.d.id)
            else App.togglePause(row.d.id)
        }
    }

    // ------------------------------------------------------------ layout
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 10
        spacing: 14

        Rectangle { // file-type tile
            readonly property color c: Theme.categoryColor(row.d.category)
            Layout.preferredWidth: row.compact ? 26 : 40
            Layout.preferredHeight: Layout.preferredWidth
            radius: row.compact ? 7 : 10
            color: Qt.rgba(c.r, c.g, c.b, Theme.dark ? 0.2 : 0.12)
            Text {
                anchors.centerIn: parent
                text: Theme.categoryGlyph(row.d.category)
                font.family: Theme.iconFont
                font.pixelSize: parent.width * 0.45
                color: parent.c
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: row.d.title
                    elide: Text.ElideMiddle
                    color: Theme.text
                    font.pixelSize: 14
                    font.weight: Font.Medium
                }
                Text { // compact mode: inline status
                    visible: row.compact
                    text: row.d.running && row.d.total > 0 ? Math.floor(row.d.progress * 100) + "%" : (row.pill ? row.pill.text : "")
                    color: row.pill ? row.pill.fg : Theme.textDim
                    font.pixelSize: Theme.fontSmall
                }
            }
            ProgressTrack {
                Layout.fillWidth: true
                visible: !row.compact && row.hasProgress
                value: row.d.progress
                active: row.d.running
                failed: row.d.failed
            }
            RowLayout {
                visible: !row.compact
                Layout.fillWidth: true
                spacing: 8
                StatePill {
                    visible: row.pill !== null
                    text: row.pill ? row.pill.text : ""
                    glyph: row.pill ? row.pill.glyph : ""
                    fg: row.pill ? row.pill.fg : Theme.textDim
                    bg: row.pill ? row.pill.bg : Theme.surfaceAlt
                }
                Text {
                    Layout.fillWidth: true
                    text: row.metaLine()
                    elide: Text.ElideRight
                    color: row.d.failed ? Theme.error : Theme.textDim
                    font.pixelSize: Theme.fontSmall
                }
            }
        }

        ColumnLayout { // speed / time left
            Layout.preferredWidth: 104
            spacing: 2
            Text {
                Layout.alignment: Qt.AlignRight
                text: row.d.running && row.d.speed > 0 ? App.fmtSpeed(row.d.speed)
                     : (row.d.seeding && row.d.uploadSpeed > 0 ? "↑ " + App.fmtSpeed(row.d.uploadSpeed) : "")
                color: Theme.accent
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
            }
            Text {
                Layout.alignment: Qt.AlignRight
                visible: !row.compact && row.d.running && row.d.eta >= 0
                text: App.fmtEta(row.d.eta) + " " + qsTr("left")
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
        }

        ColumnLayout { // size / percent
            Layout.preferredWidth: 78
            spacing: 2
            Text {
                Layout.alignment: Qt.AlignRight
                text: row.d.total >= 0 ? App.fmtSize(row.d.total) : ""
                color: Theme.text
                font.pixelSize: Theme.fontBody
            }
            Text {
                Layout.alignment: Qt.AlignRight
                visible: !row.compact && row.hasProgress && row.d.progress >= 0
                text: Math.floor(row.d.progress * 100) + "%"
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
        }

        Row { // actions (hover / selection)
            spacing: 2
            opacity: row.showActions ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 100 } }
            IconButton {
                glyph: (row.d.completed && row.d.seeding) ? Theme.icons.upload
                     : (row.d.completed && row.d.fileMissing) ? Theme.icons.refresh
                     : row.d.completed ? Theme.icons.open
                     : (row.d.running || row.d.queued) ? Theme.icons.pause
                     : row.d.failed ? Theme.icons.refresh : Theme.icons.play
                tip: (row.d.completed && row.d.seeding) ? qsTr("Stop seeding")
                     : (row.d.completed && row.d.fileMissing) ? qsTr("Download again")
                     : row.d.completed ? qsTr("Open") : (row.d.running || row.d.queued) ? qsTr("Pause")
                     : row.d.failed ? qsTr("Retry") : qsTr("Resume")
                onClicked: (row.d.completed && row.d.fileMissing) ? App.restart(row.d.id)
                         : (row.d.completed && !row.d.seeding) ? App.openFile(row.d.id) : App.togglePause(row.d.id)
            }
            IconButton {
                glyph: Theme.icons.folder
                tip: qsTr("Show in folder")
                onClicked: App.openFolder(row.d.id)
            }
            IconButton {
                glyph: String.fromCharCode(0xE712) // More
                tip: qsTr("More")
                onClicked: { App.select(row.d.id); ctx.popup() }
            }
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        height: 1
        color: Theme.border
        opacity: 0.55
        visible: !row.isSelected
    }

    // Finished, on disk and not being remuxed/moved: "Open" has something to act on.
    readonly property bool canOpen: d.completed && !d.fileMissing && !d.converting && !d.moving

    // Developer aid (screenshots): show this row's context menu.
    function showMenu() { (App.selectionCount > 1 ? multiCtx : ctx).popup(row, row.width - 300, row.height / 2) }

    FMenu {
        id: multiCtx
        FMenuItem { text: qsTr("%1 downloads selected").arg(App.selectionCount); enabled: false }
        FMenuSeparator {}
        FMenuItem { glyph: Theme.icons.open; text: qsTr("Open"); onTriggered: App.openSelected() }
        FMenuItem { glyph: Theme.icons.play; text: qsTr("Resume"); onTriggered: App.resumeSelected() }
        FMenuItem { glyph: Theme.icons.pause; text: qsTr("Pause"); onTriggered: App.pauseSelected() }
        FMenu {
            title: qsTr("Priority")
            FMenuItem { text: qsTr("High"); onTriggered: App.setPrioritySelected(2) }
            FMenuItem { text: qsTr("Normal"); onTriggered: App.setPrioritySelected(1) }
            FMenuItem { text: qsTr("Low"); onTriggered: App.setPrioritySelected(0) }
        }
        FMenuSeparator {}
        FMenuItem { glyph: Theme.icons.copy; text: qsTr("Copy links"); onTriggered: App.copySelectedLinks() }
        FMenuItem { glyph: String.fromCharCode(0xE8DE); text: qsTr("Move to…"); onTriggered: row.requestMove() }
        FMenuSeparator {}
        FMenuItem { glyph: Theme.icons.del; danger: true; text: qsTr("Delete %1 downloads…").arg(App.selectionCount); onTriggered: row.requestDeleteSelection() }
    }

    FMenu {
        id: ctx
        FMenuItem { glyph: Theme.icons.open; text: qsTr("Open"); enabled: row.canOpen; onTriggered: App.openFile(row.d.id) }
        FMenuItem {
            glyph: String.fromCharCode(0xE7AC) // OpenWith
            text: qsTr("Open with…")
            enabled: row.canOpen
            onTriggered: App.openWith(row.d.id)
        }
        FMenuItem { glyph: Theme.icons.folder; text: qsTr("Show in folder"); onTriggered: App.openFolder(row.d.id) }
        FMenuItem {
            glyph: Theme.icons.globe
            text: qsTr("Open download page")
            visible: App.hasPage(row.d.id)
            onTriggered: App.openPage(row.d.id)
        }
        FMenuSeparator {}
        FMenuItem {
            glyph: (row.d.running || row.d.queued) ? Theme.icons.pause : Theme.icons.play
            text: (row.d.running || row.d.queued) ? qsTr("Pause") : qsTr("Resume")
            visible: !row.d.completed
            onTriggered: App.togglePause(row.d.id)
        }
        FMenuItem {
            glyph: Theme.icons.upload
            text: qsTr("Stop seeding")
            visible: row.d.seeding
            onTriggered: App.stopSeeding(row.d.id)
        }
        FMenuItem {
            glyph: Theme.icons.clock
            text: row.d.scheduled ? qsTr("Edit schedule…") : qsTr("Schedule…")
            visible: !row.d.completed
            onTriggered: row.requestSchedule(row.d)
        }
        FMenuItem {
            glyph: Theme.icons.bolt
            text: row.d.speedLimit > 0 ? qsTr("Speed limit (%1)…").arg(App.fmtSpeed(row.d.speedLimit * 1024)) : qsTr("Speed limit…")
            visible: !row.d.completed
            onTriggered: row.requestSpeedLimit(row.d)
        }
        FMenuItem { glyph: Theme.icons.refresh; text: row.d.fileMissing ? qsTr("Download again") : qsTr("Restart"); onTriggered: App.restart(row.d.id) }
        FMenuSeparator {}
        FMenuItem {
            glyph: String.fromCharCode(0xE8AC) // Rename
            text: qsTr("Rename…")
            visible: !row.d.isTorrent
            enabled: row.canOpen
            onTriggered: row.requestRename(row.d)
        }
        FMenuItem {
            glyph: String.fromCharCode(0xE8DE) // MoveToFolder
            text: qsTr("Move to…")
            enabled: !row.d.running && !row.d.seeding && !row.d.moving
            onTriggered: row.requestMove()
        }
        FMenuItem { glyph: Theme.icons.copy; text: qsTr("Copy link"); onTriggered: App.copyLink(row.d.id) }
        FMenuItem {
            glyph: String.fromCharCode(0xE8C8) // Copy
            text: qsTr("Copy file path")
            enabled: row.d.filePath !== "" || row.d.destDir !== ""
            onTriggered: App.copyPath(row.d.id)
        }
        FMenu {
            title: qsTr("Priority")
            FMenuItem { text: qsTr("High"); checkable: true; checked: row.d.priority === 2; onTriggered: App.setPriority(row.d.id, 2) }
            FMenuItem { text: qsTr("Normal"); checkable: true; checked: row.d.priority === 1; onTriggered: App.setPriority(row.d.id, 1) }
            FMenuItem { text: qsTr("Low"); checkable: true; checked: row.d.priority === 0; onTriggered: App.setPriority(row.d.id, 0) }
        }
        FMenuSeparator {}
        FMenuItem { glyph: Theme.icons.del; danger: true; text: qsTr("Delete…"); onTriggered: row.requestDelete(row.d) }
    }
}
