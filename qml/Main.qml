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
        function onWhenDoneCountdown(action) {
            win.bringToFront()
            whenDoneDialog.start(action)
        }
    }

    readonly property bool dialogOpen: addDialog.visible || settingsDialog.visible || deleteDialog.visible || aboutDialog.visible || scheduleDialog.visible || whenDoneDialog.visible

    Shortcut { sequence: "Ctrl+N"; enabled: !win.dialogOpen; onActivated: addDialog.openWith(App.clipboardUrls()) }
    Shortcut { sequences: [StandardKey.Paste]; enabled: !win.dialogOpen; onActivated: addDialog.openWith(App.clipboardUrls()) }
    Shortcut { sequence: StandardKey.Delete; enabled: !win.dialogOpen && App.selected !== null; onActivated: deleteDialog.openFor(App.selected) }
    Shortcut { sequence: StandardKey.Find; onActivated: topBar.focusSearch() }

    header: TopBar {
        id: topBar
        onAddClicked: addDialog.openWith(App.clipboardUrls())
        onDeleteClicked: if (App.selected) deleteDialog.openFor(App.selected)
        onSettingsClicked: settingsDialog.open()
        onAboutClicked: aboutDialog.open()
        onOpenTorrentClicked: torrentFileDialog.open()
    }

    footer: StatusBar {}

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        FilterBar { Layout.fillWidth: true }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            handle: Rectangle {
                implicitHeight: 4
                color: SplitHandle.pressed ? Theme.accent : (SplitHandle.hovered ? Theme.border : "transparent")
            }

            DownloadsList {
                SplitView.fillHeight: true
                SplitView.minimumHeight: 120
                onRequestDelete: (d) => deleteDialog.openFor(d)
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

    Component.onCompleted: {
        // Developer aid for screenshots (FLUXDM_DEV_OPEN): open a view at startup.
        const v = App.devOpen
        if (v === "") return
        if (v === "panel" || v === "panel-log" || v === "panel-files") {
            App.select(App.downloads.idAt(0))
            App.bottomPanelOpen = true
            App.bottomPanelTab = v === "panel-log" ? 2 : (v === "panel-files" ? 1 : 0)
        } else if (v.startsWith("settings")) {
            settingsDialog.openSection(["settings", "settings-downloads", "settings-traffic", "settings-schedule", "settings-bt", "settings-browser"].indexOf(v))
        }
        else if (v === "add") addDialog.openWith("https://example.com/part[01-12].zip")
        else if (v === "menu") topBar.openMenu()
        else if (v === "delete") { App.select(App.downloads.idAt(5)); deleteDialog.openFor(App.selected) }
        else if (v === "schedule") { App.select(App.downloads.idAt(2)); scheduleDialog.openFor(App.selected) }
        else if (v === "whendone") whenDoneDialog.start(4) // "Exit FluxDM": never a real power action in dev mode
        else if (v === "about") aboutDialog.open()
        else if (v === "compact") App.compactView = true
        else if (v === "narrow") { win.width = 820; win.height = 560 }
    }

    AddDownloadDialog { id: addDialog }
    SettingsPage { id: settingsDialog }
    DeleteDialog { id: deleteDialog }
    AboutDialog { id: aboutDialog }
    ScheduleDialog { id: scheduleDialog }
    WhenDoneDialog { id: whenDoneDialog }
    FileDialog {
        id: torrentFileDialog
        title: qsTr("Open torrent file")
        nameFilters: [qsTr("Torrent files (*.torrent)")]
        fileMode: FileDialog.OpenFiles
        onAccepted: addDialog.openWith(selectedFiles.map(u => App.urlToLocal(u)).join("\n"))
    }
}
