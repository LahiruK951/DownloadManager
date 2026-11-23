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
    signal requestLogin(var download)
    signal requestExisting(var download)
    signal requestFiles(var download)
    signal requestNewTag(var downloadIds)
    signal requestConvert(var download, string format)
    signal requestCertificate(var download)
    signal requestMirrors(var download)
    signal requestChangeAddress(var download)
    signal requestWindow(var download)

    readonly property var d: download
    Accessible.role: Accessible.ListItem
    Accessible.name: d.title + ", " + (d.seeding ? qsTr("Seeding") : d.stateText)
                     + (d.total > 0 && !d.completed ? ", " + Math.floor(d.progress * 100) + "%" : "")
    Accessible.selected: isSelected
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
        if (d.converting) return { text: d.convertProgress >= 0 ? qsTr("Converting… %1%").arg(Math.floor(d.convertProgress * 100)) : qsTr("Converting…"), glyph: Theme.icons.refresh, fg: Theme.accent, bg: Theme.accentSoft }
        if (d.moving) return { text: qsTr("Moving…"), glyph: Theme.icons.folder, fg: Theme.accent, bg: Theme.accentSoft }
        if (d.completed && d.seeding) return { text: qsTr("Seeding"), glyph: Theme.icons.upload, fg: Theme.accent, bg: Theme.accentSoft }
        if (d.completed && d.fileMissing) return { text: qsTr("File missing"), glyph: Theme.icons.warn, fg: Theme.warning, bg: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.15) }
        if (d.completed) return { text: qsTr("Done"), glyph: Theme.icons.check, fg: Theme.success, bg: Qt.rgba(Theme.success.r, Theme.success.g, Theme.success.b, 0.14) }
        if (d.awaitingFiles) return { text: qsTr("Choose files"), glyph: Theme.icons.check, fg: Theme.accent, bg: Theme.accentSoft }
        if (d.certificateProblem) return { text: qsTr("Certificate problem"), glyph: Theme.icons.warn, fg: Theme.warning, bg: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.15) }
        if (d.fileExists) return { text: qsTr("File exists"), glyph: Theme.icons.warn, fg: Theme.warning, bg: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.15) }
        if (d.needsLogin) return { text: qsTr("Sign-in needed"), glyph: String.fromCharCode(0xE8D7), fg: Theme.warning, bg: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.15) }
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
                Repeater { // user tags
                    model: (App.tags, row.d.tags)
                    delegate: Rectangle {
                        required property var modelData
                        readonly property var tag: App.tags.find(t => t.id === modelData)
                        visible: tag !== undefined
                        height: 18
                        width: tagText.implicitWidth + 14
                        radius: 9
                        color: tag ? Qt.rgba(Qt.color(tag.color).r, Qt.color(tag.color).g, Qt.color(tag.color).b, Theme.dark ? 0.28 : 0.16) : "transparent"
                        Text {
                            id: tagText
                            anchors.centerIn: parent
                            text: parent.tag ? parent.tag.name : ""
                            color: parent.tag ? (Theme.dark ? Qt.lighter(parent.tag.color, 1.35) : Qt.darker(parent.tag.color, 1.25)) : Theme.text
                            font.pixelSize: 11
                            font.weight: Font.Medium
                        }
                    }
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
            IconButton { // untrusted certificate: let the user decide
                visible: row.d.certificateProblem
                glyph: Theme.icons.warn
                tip: qsTr("Certificate problem…")
                onClicked: row.requestCertificate(row.d)
            }
            IconButton { // torrent waits for the file selection
                visible: row.d.awaitingFiles
                glyph: Theme.icons.check
                tip: qsTr("Choose files…")
                onClicked: row.requestFiles(row.d)
            }
            IconButton { // "ask" found the file already there: let the user choose
                visible: row.d.fileExists
                glyph: Theme.icons.warn
                tip: qsTr("Replace or keep both…")
                onClicked: row.requestExisting(row.d)
            }
            IconButton { // a server asked for a login: offer it instead of a plain retry
                visible: row.d.needsLogin
                glyph: String.fromCharCode(0xE8D7) // Permissions (key)
                tip: qsTr("Sign in…")
                onClicked: row.requestLogin(row.d)
            }
            IconButton { // the link expired or was refused: a fresh one continues the download
                visible: row.d.linkProblem && App.canChangeAddress(row.d.id)
                glyph: String.fromCharCode(0xE70F) // Edit
                tip: qsTr("Change address…")
                onClicked: row.requestChangeAddress(row.d)
            }
            IconButton {
                visible: !row.d.needsLogin && !row.d.fileExists && !row.d.awaitingFiles && !row.d.certificateProblem
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
            title: qsTr("Queue")
            FMenuItem { text: qsTr("Move to top"); onTriggered: App.moveSelectionInQueue("top") }
            FMenuItem { text: qsTr("Move up"); onTriggered: App.moveSelectionInQueue("up") }
            FMenuItem { text: qsTr("Move down"); onTriggered: App.moveSelectionInQueue("down") }
            FMenuItem { text: qsTr("Move to bottom"); onTriggered: App.moveSelectionInQueue("bottom") }
        }
        FMenu {
            title: qsTr("Priority")
            FMenuItem { text: qsTr("High"); onTriggered: App.setPrioritySelected(2) }
            FMenuItem { text: qsTr("Normal"); onTriggered: App.setPrioritySelected(1) }
            FMenuItem { text: qsTr("Low"); onTriggered: App.setPrioritySelected(0) }
        }
        FMenuSeparator {}
        FMenu {
            id: multiTagsMenu
            title: qsTr("Tags")
            Instantiator {
                model: App.tags
                delegate: FMenuItem {
                    required property var modelData
                    readonly property int state: (App.selectionRev, App.selectionTagState(modelData.id))
                    text: modelData.name + (state === 1 ? "  (" + qsTr("some") + ")" : "")
                    checkable: true
                    checked: state === 2
                    onTriggered: App.setSelectionTag(modelData.id, state !== 2)
                }
                onObjectAdded: (index, object) => multiTagsMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => multiTagsMenu.removeItem(object)
            }
            FMenuSeparator { visible: App.tags.length > 0 }
            FMenuItem { text: qsTr("New tag…"); onTriggered: row.requestNewTag(App.selectedIds()) }
        }
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
        FMenuItem {
            glyph: String.fromCharCode(0xE768) // Play
            text: qsTr("Play while downloading")
            visible: (row.d.running, row.d.files, App.canPlayWhileDownloading(row.d.id))
            onTriggered: App.playWhileDownloading(row.d.id)
        }
        FMenuItem { glyph: Theme.icons.folder; text: qsTr("Show in folder"); onTriggered: App.openFolder(row.d.id) }
        FMenuItem { glyph: String.fromCharCode(0xE8A7); text: qsTr("Open in a new window"); onTriggered: row.requestWindow(row.d) }
        FMenuItem {
            glyph: String.fromCharCode(0xE8D6) // Audio
            text: qsTr("Convert to MP3…")
            visible: (row.d.completed, row.d.converting, row.d.fileMissing, App.canConvert(row.d.id, "mp3"))
            onTriggered: row.requestConvert(row.d, "mp3")
        }
        FMenuItem {
            glyph: String.fromCharCode(0xE714) // Video
            text: qsTr("Convert to MP4…")
            visible: (row.d.completed, row.d.converting, row.d.fileMissing, App.canConvert(row.d.id, "mp4"))
            onTriggered: row.requestConvert(row.d, "mp4")
        }
        FMenuItem {
            glyph: Theme.icons.folder
            text: qsTr("Extract here")
            visible: (row.d.completed, row.d.converting, row.d.fileMissing, App.canExtract(row.d.id))
            onTriggered: { const err = App.extract(row.d.id); if (err !== "") App.notice(err) }
        }
        FMenuItem {
            glyph: Theme.icons.link
            text: qsTr("Create torrent and share")
            visible: (row.d.completed, row.d.converting, row.d.fileMissing, App.canCreateTorrent(row.d.id))
            onTriggered: { const err = App.createTorrent(row.d.id); if (err !== "") App.notice(err) }
        }
        FMenuItem {
            readonly property string page: (row.d.url, row.d.referrer, App.pageUrl(row.d.id))
            glyph: Theme.icons.globe
            text: App.hasPage(row.d.id) ? qsTr("Open download page") : qsTr("Open website (%1)").arg(page.replace(/^https?:\/\//, "").replace(/\/$/, ""))
            visible: page !== ""
            onTriggered: App.openPage(row.d.id)
        }
        FMenuSeparator {}
        FMenuItem {
            glyph: Theme.icons.warn
            text: qsTr("Certificate problem…")
            visible: row.d.certificateProblem
            onTriggered: row.requestCertificate(row.d)
        }
        FMenuItem {
            glyph: Theme.icons.check
            text: qsTr("Choose files…")
            visible: row.d.awaitingFiles
            onTriggered: row.requestFiles(row.d)
        }
        FMenuItem {
            glyph: Theme.icons.warn
            text: qsTr("Replace or keep both…")
            visible: row.d.fileExists
            onTriggered: row.requestExisting(row.d)
        }
        FMenuItem {
            glyph: String.fromCharCode(0xE8D7) // Permissions (key)
            text: qsTr("Sign in…")
            visible: row.d.needsLogin
            onTriggered: row.requestLogin(row.d)
        }
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
            text: qsTr("Download in order")
            checkable: true
            checked: row.d.sequential
            visible: row.d.isTorrent && !row.d.completed
            onTriggered: App.setSequential(row.d.id, !row.d.sequential)
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
            glyph: String.fromCharCode(0xE70F) // Edit
            text: qsTr("Change address…")
            visible: (row.d.state, row.d.url, App.canChangeAddress(row.d.id))
            onTriggered: row.requestChangeAddress(row.d)
        }
        FMenuItem {
            glyph: Theme.icons.link
            text: qsTr("Mirrors…")
            visible: !row.d.completed && row.d.url.match(/^(https?|ftp):/i) !== null && !row.d.isTorrent
            onTriggered: row.requestMirrors(row.d)
        }
        FMenuItem {
            glyph: String.fromCharCode(0xE8C8) // Copy
            text: qsTr("Copy file path")
            enabled: row.d.filePath !== "" || row.d.destDir !== ""
            onTriggered: App.copyPath(row.d.id)
        }
        FMenu {
            id: tagsMenu
            title: qsTr("Tags")
            Instantiator {
                model: App.tags
                delegate: FMenuItem {
                    required property var modelData
                    text: modelData.name
                    checkable: true
                    checked: row.d.tags.indexOf(modelData.id) >= 0
                    onTriggered: App.setTag(row.d.id, modelData.id, !(row.d.tags.indexOf(modelData.id) >= 0))
                }
                onObjectAdded: (index, object) => tagsMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => tagsMenu.removeItem(object)
            }
            FMenuSeparator { visible: App.tags.length > 0 }
            FMenuItem { text: qsTr("New tag…"); onTriggered: row.requestNewTag([row.d.id]) }
        }
        FMenu {
            title: qsTr("Queue")
            FMenuItem { text: qsTr("Move to top"); onTriggered: App.moveInQueue(row.d.id, "top") }
            FMenuItem { text: qsTr("Move up"); onTriggered: App.moveInQueue(row.d.id, "up") }
            FMenuItem { text: qsTr("Move down"); onTriggered: App.moveInQueue(row.d.id, "down") }
            FMenuItem { text: qsTr("Move to bottom"); onTriggered: App.moveInQueue(row.d.id, "bottom") }
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
