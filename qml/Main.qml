import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import QtQuick.Dialogs

ApplicationWindow {
    id: win
    width: 1180
    height: 680
    minimumWidth: 760
    minimumHeight: 440
    visible: !App.startHidden
    title: "FluxDM"
    color: Theme.bg

    palette {
        window: Theme.surface
        windowText: Theme.text
        base: Theme.surfaceAlt
        alternateBase: Theme.surface
        text: Theme.text
        button: Theme.surfaceAlt
        buttonText: Theme.text
        highlight: Theme.accent
        highlightedText: "white"
        placeholderText: Theme.textDisabled
        light: Theme.surfaceAlt
        midlight: Theme.border
        mid: Theme.border
        dark: Theme.textDim
        shadow: "#000000"
        toolTipBase: Theme.surface
        toolTipText: Theme.text
    }

    Settings {
        location: App.settingsFileUrl
        category: "appWindow"
        property alias x: win.x
        property alias y: win.y
        property alias width: win.width
        property alias height: win.height
    }

    onClosing: (close) => {
        if (App.closeButtonHidesApp) {
            close.accepted = false
            win.hide()
            App.notifyHiddenToTray()
        } else {
            App.quit()
        }
    }

    function bringToFront() {
        if (win.visibility === Window.Minimized || !win.visible)
            win.showNormal()
        win.raise()
        win.requestActivate()
    }

    Connections {
        target: App
        function onShowWindowRequested() { win.bringToFront() }
        function onAddDialogRequested(urls) {
            win.bringToFront()
            addDialog.openWith(urls)
        }
        function onNotice(text) { toast.show(text) }
        // Ask right away, like a browser does, unless the user is busy with another dialog.
        function onFilesRequired(d) { if (!win.dialogOpen) torrentFilesDialog.openFor(d) }
        function onCertificateRequired(d) { if (!win.dialogOpen) certificateDialog.openFor(d) }
        function onMeteredQuestion(n) { if (!win.dialogOpen) meteredDialog.ask(n) }
        function onFileConflict(d) { if (!win.dialogOpen) fileExistsDialog.openFor(d) }
        function onLoginRequired(d) { if (!win.dialogOpen && App.devOpen !== "login-row") loginDialog.openFor(d) }
        function onWhenDoneCountdown(action) {
            win.bringToFront()
            whenDoneDialog.start(action)
        }
    }

    readonly property bool dialogOpen: addDialog.visible || settingsDialog.visible || deleteDialog.visible || aboutDialog.visible || scheduleDialog.visible || whenDoneDialog.visible || speedLimitDialog.visible || renameDialog.visible || loginDialog.visible || fileExistsDialog.visible || torrentFilesDialog.visible || tagDialog.visible || convertDialog.visible || reportDialog.visible || certificateDialog.visible || meteredDialog.visible || mirrorsDialog.visible

    Shortcut { sequence: "Ctrl+N"; enabled: !win.dialogOpen; onActivated: addDialog.openWith(App.clipboardUrls()) }
    Shortcut { sequences: [StandardKey.Paste]; enabled: !win.dialogOpen; onActivated: addDialog.openWith(App.clipboardUrls()) }
    Shortcut { sequence: StandardKey.Delete; enabled: !win.dialogOpen && App.selectionCount > 0; onActivated: deleteDialog.openForSelection() }
    Shortcut { sequence: StandardKey.SelectAll; enabled: !win.dialogOpen && !topBar.searchFocused; onActivated: App.selectAll() }
    Shortcut { sequence: "Esc"; enabled: !win.dialogOpen && App.selectionCount > 1; onActivated: App.clearSelection() }
    Shortcut { sequence: StandardKey.Find; onActivated: topBar.focusSearch() }
    Shortcut { sequence: "Ctrl+Shift+P"; enabled: !win.dialogOpen; onActivated: App.pauseAll() }
    Shortcut { sequence: "Ctrl+Shift+R"; enabled: !win.dialogOpen; onActivated: App.resumeAll() }
    Shortcut { sequence: "Ctrl+E"; enabled: !win.dialogOpen && App.selected !== null; onActivated: App.openFolder(App.selected.id) }
    Shortcut { sequence: "Ctrl+I"; enabled: !win.dialogOpen; onActivated: App.bottomPanelOpen = !App.bottomPanelOpen }
    Shortcut { sequence: "Ctrl+,"; enabled: !win.dialogOpen; onActivated: settingsDialog.open() }
    Shortcut { sequence: "Ctrl+1"; enabled: !win.dialogOpen; onActivated: App.trafficMode = 0 }
    Shortcut { sequence: "Ctrl+2"; enabled: !win.dialogOpen; onActivated: App.trafficMode = 1 }
    Shortcut { sequence: "Ctrl+3"; enabled: !win.dialogOpen; onActivated: App.trafficMode = 2 }

    header: TopBar {
        id: topBar
        onAddClicked: addDialog.openWith(App.clipboardUrls())
        onDeleteClicked: deleteDialog.openForSelection()
        onSettingsClicked: settingsDialog.open()
        onAboutClicked: aboutDialog.open()
        onReportClicked: reportDialog.openReport(false)
        onFdmImportClicked: toast.show(App.importFromFdm())
        onOpenTorrentClicked: torrentFileDialog.open()
        onImportClicked: importDialog.open()
        onExportClicked: exportDialog.open()
    }

    footer: StatusBar {}

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        FilterBar {
            id: filterBar
            Layout.fillWidth: true
            onNewTagRequested: tagDialog.openNew([])
            onEditTagRequested: (t) => tagDialog.openEdit(t)
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            handle: Rectangle {
                implicitHeight: 4
                color: SplitHandle.pressed ? Theme.accent : (SplitHandle.hovered ? Theme.border : "transparent")
            }

            DownloadsList {
                id: downloadsList
                SplitView.fillHeight: true
                SplitView.minimumHeight: 120
                onRequestDelete: (d) => deleteDialog.openFor(d)
                onRequestDeleteSelection: deleteDialog.openForSelection()
                onRequestSpeedLimit: (d) => speedLimitDialog.openFor(d)
                onRequestMove: moveDialog.open()
                onRequestRename: (d) => renameDialog.openFor(d)
                onRequestLogin: (d) => loginDialog.openFor(d)
                onRequestExisting: (d) => fileExistsDialog.openFor(d)
                onRequestFiles: (d) => torrentFilesDialog.openFor(d)
                onRequestNewTag: (ids) => tagDialog.openNew(ids)
                onRequestConvert: (d, f) => convertDialog.openFor(d, f)
                onRequestCertificate: (d) => certificateDialog.openFor(d)
                onRequestMirrors: (d) => mirrorsDialog.openFor(d)
                onRequestWindow: (d) => win.openDownloadWindow(d)
                onRequestSchedule: (d) => scheduleDialog.openFor(d)
                onRequestAdd: addDialog.openWith(App.clipboardUrls())
                onRequestOpenTorrent: torrentFileDialog.open()
            }

            BottomPanel {
                id: bottomPanel
                visible: App.bottomPanelOpen && App.selected !== null
                download: App.selected
                SplitView.preferredHeight: App.bottomPanelHeight
                SplitView.minimumHeight: 140
                onCloseRequested: App.bottomPanelOpen = false
                onPopOutRequested: win.openDownloadWindow(App.selected)
                onHeightChanged: if (visible && height > 0) App.bottomPanelHeight = height
            }
        }
    }

    DropArea {
        anchors.fill: parent
        keys: ["text/uri-list", "text/plain"]
        onDropped: (drop) => {
            const text = drop.hasUrls ? drop.urls.map(u => u.toString()).join("\n") : drop.text
            addDialog.openWith(text)
            drop.accept()
        }
    }

    // Separate windows per download; an already open one is brought to the front.
    property var downloadWindows: ({})
    Component { id: downloadWindowComponent; DownloadWindow {} }
    function openDownloadWindow(d) {
        if (!d) return
        const w = downloadWindows[d.id]
        if (w) { w.raise(); w.requestActivate(); return }
        const nw = downloadWindowComponent.createObject(null, { download: d })
        downloadWindows[d.id] = nw
        nw.closing.connect(() => delete downloadWindows[d.id])
    }

    Component.onCompleted: {
        // Developer aid for screenshots (FLUXDM_DEV_OPEN): open a view at startup.
        const v = App.devOpen
        if (v === "" && App.previousSessionCrashed) crashTimer.start() // offer a report after a crash
        if (v === "") return
        if (v === "panel" || v === "panel-log" || v === "panel-files") {
            App.select(App.downloads.idAt(0))
            App.bottomPanelOpen = true
            App.bottomPanelTab = v === "panel-log" ? 2 : (v === "panel-files" ? 1 : 0)
        } else if (v.startsWith("settings")) {
            settingsDialog.openSection(["settings", "settings-downloads", "settings-traffic", "settings-schedule", "settings-bt", "settings-browser", "settings-network", "settings-remote"].indexOf(v))
        }
        else if (v === "add") addDialog.openWith("https://example.com/part[01-12].zip")
        else if (v === "add-dup") { App.select(App.downloads.idAt(1)); addDialog.openWith(App.selected ? App.selected.url : "") }
        else if (v === "add-hls") addDialog.openWith("https://media.example.com/shows/night-drive/master.m3u8")
        else if (v === "menu") topBar.openMenu()
        else if (v === "delete") { App.select(App.downloads.idAt(5)); deleteDialog.openFor(App.selected) }
        else if (v === "schedule") { App.select(App.downloads.idAt(2)); scheduleDialog.openFor(App.selected) }
        else if (v === "whendone") whenDoneDialog.start(4) // "Exit FluxDM": never a real power action in dev mode
        else if (v === "about") aboutDialog.open()
        else if (v === "compact") App.compactView = true
        else if (v.startsWith("move:")) devMoveTimer.start()
        else if (v === "speedlimit") { App.select(App.downloads.idAt(0)); speedLimitDialog.openFor(App.selected) }
        else if (v === "multi") { App.select(App.downloads.idAt(0)); App.toggleSelect(App.downloads.idAt(1)); App.toggleSelect(App.downloads.idAt(3)) }
        else if (v === "toast") devToastTimer.start()
        else if (v.startsWith("ctx")) devMenuTimer.start() // ctx[:<row>], ctx-multi
        else if (v === "network-manual") { // throwaway profile only
            App.setProxySettings(2, "socks5", "proxy.example.com", 1080, "me", "secret", false)
            settingsDialog.openSection(6)
        }
        else if (v === "report") reportDialog.openReport(false)
        else if (v === "metered") meteredDialog.ask(2)
        else if (v === "window") { App.select(App.downloads.idAt(0)); openDownloadWindow(App.selected) }
        else if (v === "exists") devExistsTimer.start()
        else if (v === "convert") devConvertTimer.start()
        else if (v === "tags" || v === "tag-dialog" || v === "tags-menu") { // sample tags on the demo list
            const work = App.saveTag(0, "Work", "#539bf5", "pdf, xlsx", "")
            const isos = App.saveTag(0, "Linux ISOs", "#57ab5a", "iso", "")
            App.saveTag(0, "Keep", "#e275ad", "", "")
            App.setTag(App.downloads.idAt(1), work, true)
            if (v === "tag-dialog") tagDialog.openEdit(App.tags[1])
            if (v === "tags-menu") devMenuTimer.start()
        }
        else if (v === "bt-files") { // a torrent-like demo download asking which files to get
            App.select(App.downloads.idAt(0))
            App.addDownloads("demo://tracker.example.org/The%20Long%20Night%20S01?size=4551000000&speed=6000000&bt=1&pick=1", App.selected.destDir, "", true, {})
        }
        else if (v === "bt-peers" || v === "bt-trackers") {
            App.select(App.downloads.idAt(0))
            App.addDownloads("demo://tracker.example.org/Open%20Movie%20Pack?size=2147483648&speed=3700000&start=0.35&bt=1", App.selected.destDir, "", true, {})
            App.bottomPanelOpen = true
            App.bottomPanelTab = v === "bt-peers" ? 3 : 4
        }
        else if (v.startsWith("sort:")) App.setSort(v.split(":")[1], v.split(":")[2] === "desc") // sort:<key>:<asc|desc>
        else if (v === "sort-menu") devSortTimer.start()
        else if (v === "login" || v === "login-row") {
            App.select(App.downloads.idAt(0)) // reuse the demo profile's folder, never the real default
            App.addDownloads("demo://intranet.example.com/reports/q3-figures.xlsx?size=2400000&fail=401", App.selected.destDir, "", true, {})
        }
        else if (v.startsWith("rename")) { App.select(App.downloads.idAt(Number(v.split(":")[1] || 0))); renameDialog.openFor(App.selected) }
        else if (v.startsWith("export:")) toast.show(App.exportDownloads(App.toUrl(v.substring(7))))
        else if (v.startsWith("import:")) { toast.show(App.importDownloads(App.toUrl(v.substring(7)))); App.filter = "active" }
        else if (v.startsWith("search:")) App.search = v.substring(7)
        else if (v === "narrow") { win.width = 820; win.height = 560 }
    }

    AddDownloadDialog { id: addDialog }
    SettingsPage { id: settingsDialog }
    DeleteDialog { id: deleteDialog }
    AboutDialog { id: aboutDialog }
    ScheduleDialog { id: scheduleDialog }
    SpeedLimitDialog { id: speedLimitDialog }
    RenameDialog { id: renameDialog }
    LoginDialog { id: loginDialog }
    FileExistsDialog { id: fileExistsDialog }
    TorrentFilesDialog { id: torrentFilesDialog }
    TagDialog { id: tagDialog }
    ConvertDialog { id: convertDialog }
    ReportDialog { id: reportDialog }
    CertificateDialog { id: certificateDialog }
    MeteredDialog { id: meteredDialog }
    MirrorsDialog { id: mirrorsDialog }
    WhenDoneDialog { id: whenDoneDialog }
    FileDialog {
        id: importDialog
        title: qsTr("Import downloads")
        nameFilters: [qsTr("Download lists (*.json *.txt)"), qsTr("All files (*)")]
        onAccepted: toast.show(App.importDownloads(selectedFile))
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export downloads")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: [qsTr("FluxDM download list (*.json)")]
        onAccepted: toast.show(App.exportDownloads(selectedFile))
    }
    FolderDialog {
        id: moveDialog
        title: qsTr("Move to folder")
        onAccepted: {
            const err = App.moveSelected(selectedFolder)
            toast.show(err !== "" ? err : qsTr("Moved to %1").arg(App.urlToLocal(selectedFolder)))
        }
    }
    Toast { id: toast }
    Timer {
        id: devMoveTimer
        interval: 1500
        onTriggered: {
            App.select(App.downloads.idAt(6))
            App.bottomPanelOpen = true
            const e = App.moveSelected(App.toUrl(App.devOpen.substring(5)))
            toast.show(e !== "" ? e : qsTr("Moved"))
        }
    }
    Timer {
        id: devMenuTimer
        interval: 600 // rows must exist before their menu can be shown
        onTriggered: {
            const v = App.devOpen
            const i = v === "ctx-multi" ? 0 : Number(v.split(":")[1] || 0)
            App.select(App.downloads.idAt(i))
            if (v === "ctx-multi") { App.toggleSelect(App.downloads.idAt(1)); App.toggleSelect(App.downloads.idAt(2)) }
            downloadsList.showContextMenu(i)
        }
    }
    Timer {
        id: devExistsTimer
        interval: 1200 // demo downloads write their placeholder files when they finish
        onTriggered: {
            App.select(App.downloads.idAt(5))
            addDialog.openWith("https://downloads.example.com/" + App.selected.title, App.selected.destDir)
        }
    }
    Timer {
        id: devConvertTimer
        interval: 1200 // demo files are finished (placeholder on disk) by then
        onTriggered: { // the finished .flac in the demo list
            for (let i = 0; i < App.downloads.count; ++i) {
                App.select(App.downloads.idAt(i))
                if (App.selected.title.endsWith(".flac") && App.selected.completed) break
            }
            convertDialog.openFor(App.selected, "mp3")
        }
    }
    Timer { id: crashTimer; interval: 800; onTriggered: if (!win.dialogOpen) reportDialog.openReport(true) }
    Timer { id: devSortTimer; interval: 600; onTriggered: filterBar.openSortMenu() }
    Timer { id: devToastTimer; interval: 1200; onTriggered: toast.show(qsTr("Imported 12 download(s) (paused).")) }
    FileDialog {
        id: torrentFileDialog
        title: qsTr("Open torrent file")
        nameFilters: [qsTr("Torrent files (*.torrent)")]
        fileMode: FileDialog.OpenFiles
        onAccepted: addDialog.openWith(selectedFiles.map(u => App.urlToLocal(u)).join("\n"))
    }
}
