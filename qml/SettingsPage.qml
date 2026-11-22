import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Full-window settings (FDM 6 style): section sidebar, cards, changes apply instantly.
Popup {
    id: page
    property int section: 0
    property var tum: []
    property var rules: []
    property int folderRev: 0
    property string ffmpeg: ""
    property string ytdlp: ""
    readonly property var modeNames: [qsTr("Snail"), qsTr("Medium"), qsTr("High")]
    readonly property var sections: [
        { title: qsTr("General"), glyph: Theme.icons.settings, desc: qsTr("Appearance and how FluxDM behaves on your desktop.") },
        { title: qsTr("Downloads"), glyph: Theme.icons.down, desc: qsTr("Where new downloads are saved.") },
        { title: qsTr("Traffic modes"), glyph: Theme.icons.bolt, desc: qsTr("Limits for each mode. Switch modes from the status bar at the bottom of the window.") },
        { title: qsTr("Snail schedule"), glyph: Theme.icons.clock, desc: qsTr("Switch traffic modes automatically — for example Snail mode during working hours.") },
        { title: qsTr("BitTorrent"), glyph: Theme.icons.link, desc: qsTr("Seeding, upload speed and file associations.") },
        { title: qsTr("Browser"), glyph: Theme.icons.globe, desc: qsTr("Send downloads from Chrome, Edge, Brave and Firefox to FluxDM.") },
        { title: qsTr("Network"), glyph: String.fromCharCode(0xE968), desc: qsTr("Proxy and how many connections FluxDM opens to one server.") },
        { title: qsTr("Remote"), glyph: String.fromCharCode(0xE8EA), desc: qsTr("Control FluxDM from your phone over the local network.") }
    ]

    function openSection(i) {
        section = i
        open()
    }

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 800
    height: parent ? parent.height : 600
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape
    Overlay.modal: Item {}

    onAboutToShow: {
        tum = [App.tumSettings(0), App.tumSettings(1), App.tumSettings(2)]
        rules = App.trafficRules()
        folderField.text = App.defaultFolder
        upField.value = App.uploadLimitKBps()
        ratioField.value = Math.round(App.seedRatio() * 10)
        assocToggle.checked = App.torrentAssociated()
        ffmpegField.text = App.ffmpegSetting()
        page.ffmpeg = App.ffmpegFound()
        ytdlpField.text = App.ytdlpSetting()
        page.ytdlp = App.ytdlpFound()
        browserSection.load()
    }

    function saveTum(m) { App.setTumSettings(m, tum[m].maxDownloads, tum[m].maxConnections, tum[m].speedLimit) }
    function saveRules() { App.setTrafficRules(rules) }
    function addRule() {
        const r = rules.slice()
        r.push({ enabled: true, days: 0x1F, from: 9 * 60, to: 18 * 60, mode: 0 }) // weekdays 09-18 → Snail
        rules = r
        saveRules()
    }
    function removeRule(i) {
        const r = rules.slice()
        r.splice(i, 1)
        rules = r
        saveRules()
    }

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 } }

    background: Rectangle { color: Theme.bg }

    contentItem: RowLayout {
        spacing: 0

        // ------------------------------------------------ sidebar
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 232
            color: Theme.surface
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.border }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 2

                RowLayout {
                    Layout.bottomMargin: 14
                    spacing: 6
                    IconButton {
                        glyph: Theme.icons.back
                        tip: qsTr("Back (Esc)")
                        onClicked: page.close()
                    }
                    Text { text: qsTr("Settings"); color: Theme.text; font.pixelSize: 18; font.weight: Font.DemiBold }
                }

                Repeater {
                    model: page.sections
                    delegate: AbstractButton {
                        id: nav
                        required property var modelData
                        required property int index
                        readonly property bool current: page.section === index
                        visible: index !== 4 || App.torrentSupport
                        Layout.fillWidth: true
                        implicitHeight: 38
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus
                        onClicked: page.section = index
                        background: Rectangle {
                            radius: Theme.radius
                            color: nav.current ? Theme.accentSoft : (nav.hovered ? Theme.hover : "transparent")
                            Rectangle {
                                visible: nav.current
                                width: 3
                                height: 16
                                radius: 2
                                anchors.verticalCenter: parent.verticalCenter
                                color: Theme.accent
                            }
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Text {
                                Layout.leftMargin: 12
                                text: nav.modelData.glyph
                                font.family: Theme.iconFont
                                font.pixelSize: 14
                                color: nav.current ? Theme.accent : Theme.textDim
                            }
                            Text {
                                Layout.fillWidth: true
                                text: nav.modelData.title
                                font.pixelSize: Theme.fontBody
                                font.weight: nav.current ? Font.DemiBold : Font.Normal
                                color: nav.current ? Theme.accent : Theme.text
                            }
                        }
                    }
                }
                Item { Layout.fillHeight: true }
                Text {
                    Layout.leftMargin: 12
                    text: "FluxDM " + App.version
                    color: Theme.textDisabled
                    font.pixelSize: Theme.fontSmall
                }
            }
        }

        // ------------------------------------------------ content
        ScrollView {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                id: content
                width: Math.min(scroller.availableWidth - 64, 780)
                x: 32
                spacing: 20

                Item { implicitHeight: 12 }
                ColumnLayout {
                    spacing: 4
                    Text {
                        text: page.sections[page.section].title
                        color: Theme.text
                        font.pixelSize: 24
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: page.sections[page.section].desc
                        color: Theme.textDim
                        font.pixelSize: Theme.fontBody
                        wrapMode: Text.WordWrap
                    }
                }

                // ---------------- General
                SettingsGeneral { visible: page.section === 0; Layout.fillWidth: true }

                // ---------------- Downloads
                SettingsCard {
                    visible: page.section === 1
                    title: qsTr("Saving")
                    Layout.fillWidth: true
                    SettingRow {
                        title: qsTr("Retry failed downloads")
                        description: qsTr("Automatic attempts after network or server errors (30 s, 1, 2, 5, 10 min apart). 0 turns it off.")
                        FSpinBox {
                            from: 0; to: 50
                            zeroText: qsTr("Off")
                            Component.onCompleted: value = App.autoRetryLimit()
                            onValueModified: App.setAutoRetryLimit(value)
                        }
                    }
                    SettingRow {
                        title: qsTr("Check finished downloads with your antivirus")
                        description: qsTr("Like a browser: Windows scans each new file and marks it as coming from the internet, so it warns before running it.")
                        FToggle { checked: App.antivirusCheck; onToggled: App.antivirusCheck = checked }
                    }
                    SettingRow {
                        title: qsTr("If the file already exists")
                        description: qsTr("For new downloads. “Replace” only removes the old file once the new one is complete.")
                        FComboBox {
                            Layout.preferredWidth: 200
                            model: [qsTr("Keep both (add a number)"), qsTr("Replace it"), qsTr("Ask me")]
                            currentIndex: (App.defaultFolder, App.existingFileDefault())
                            onActivated: (i) => App.setExistingFileDefault(i)
                        }
                    }
                    SettingRow {
                        // re-read whenever settings change (signing in with "remember" emits it)
                        id: loginsRow
                        readonly property int count: (App.defaultFolder, App.savedLoginCount())
                        title: qsTr("Saved server logins")
                        description: count === 0 ? qsTr("None. When a server asks for a user name and password, you can let FluxDM remember it.")
                                   : count === 1 ? qsTr("1 server. Passwords are encrypted for your Windows account.")
                                   : qsTr("%1 servers. Passwords are encrypted for your Windows account.").arg(count)
                        FButton {
                            text: qsTr("Forget all")
                            enabled: loginsRow.count > 0
                            onClicked: App.forgetLogins()
                        }
                    }
                    SettingRow {
                        title: qsTr("Convert video streams to MP4")
                        description: page.ffmpeg !== "" ? qsTr("Lossless remux with ffmpeg (%1).").arg(page.ffmpeg)
                                                        : qsTr("Needs ffmpeg: not found. Install it or set its path below; streams stay .ts until then.")
                        FToggle { checked: App.convertStreams(); onToggled: App.setConvertStreams(checked) }
                    }
                    SettingRow {
                        title: qsTr("ffmpeg location")
                        description: qsTr("Leave empty to look for ffmpeg next to FluxDM and on PATH.")
                        Row {
                            spacing: 8
                            FTextField {
                                id: ffmpegField
                                width: 260
                                placeholderText: qsTr("Automatic")
                                onEditingFinished: { App.setFfmpegSetting(text); page.ffmpeg = App.ffmpegFound() }
                            }
                            FButton { text: qsTr("Browse…"); onClicked: ffmpegDlg.open() }
                        }
                    }
                    SettingRow {
                        title: qsTr("yt-dlp location")
                        description: page.ytdlp !== "" ? qsTr("For videos from YouTube and other sites: %1").arg(page.ytdlp)
                                                       : qsTr("For videos from YouTube and other sites. Not found: put yt-dlp.exe next to FluxDM or on PATH, or choose it here.")
                        RowLayout {
                            spacing: 8
                            FTextField {
                                id: ytdlpField
                                implicitWidth: 260
                                placeholderText: qsTr("Automatic")
                                onEditingFinished: { App.setYtdlpSetting(text); page.ytdlp = App.ytdlpFound() }
                            }
                            FButton { text: qsTr("Browse…"); onClicked: ytdlpDlg.open() }
                        }
                    }
                    SettingRow {
                        title: qsTr("Remove finished downloads from the list")
                        description: qsTr("Only the entry in FluxDM goes; the files stay where they are.")
                        FComboBox {
                            Layout.preferredWidth: 200
                            readonly property var days: [0, 1, 7, 30, 90]
                            model: [qsTr("Never"), qsTr("After 1 day"), qsTr("After 7 days"), qsTr("After 30 days"), qsTr("After 90 days")]
                            currentIndex: Math.max(0, days.indexOf(App.autoRemoveDays))
                            onActivated: (i) => App.autoRemoveDays = days[i]
                        }
                    }
                    SettingRow {
                        title: qsTr("Default folder")
                        description: qsTr("Used for new downloads unless you pick another folder.")
                        divider: false
                        Row {
                            spacing: 8
                            FTextField {
                                id: folderField
                                width: 300
                                onEditingFinished: App.defaultFolder = text
                            }
                            FButton { text: qsTr("Browse…"); onClicked: folderDlg.open() }
                        }
                    }
                }

                SettingsCard {
                    visible: page.section === 1
                    title: qsTr("Folders by file type")
                    description: qsTr("New downloads of each type go to its folder; everything else uses the default folder.")
                    Layout.fillWidth: true
                    Repeater {
                        model: [
                            { key: "Video", label: qsTr("Video"), glyph: Theme.categoryGlyph("Video") },
                            { key: "Music", label: qsTr("Music"), glyph: Theme.categoryGlyph("Music") },
                            { key: "Archive", label: qsTr("Archives"), glyph: Theme.categoryGlyph("Archive") },
                            { key: "Docs", label: qsTr("Documents"), glyph: Theme.categoryGlyph("Docs") },
                            { key: "Programs", label: qsTr("Programs"), glyph: Theme.categoryGlyph("Programs") },
                            { key: "Pictures", label: qsTr("Pictures"), glyph: Theme.categoryGlyph("Pictures") }
                        ]
                        delegate: SettingRow {
                            required property var modelData
                            required property int index
                            readonly property string folder: (page.folderRev, App.categoryFolder(modelData.key))
                            title: modelData.label
                            description: folder !== "" ? folder : qsTr("Default folder")
                            divider: index < 5
                            Row {
                                spacing: 4
                                FButton {
                                    text: qsTr("Choose…")
                                    onClicked: { categoryDlg.category = modelData.key; categoryDlg.open() }
                                }
                                IconButton {
                                    visible: folder !== ""
                                    glyph: Theme.icons.close
                                    glyphSize: 10
                                    size: Theme.controlHeight
                                    tip: qsTr("Use the default folder")
                                    onClicked: { App.setCategoryFolder(modelData.key, ""); page.folderRev++ }
                                }
                            }
                        }
                    }
                }
                // ---------------- Traffic modes
                GridLayout {
                    visible: page.section === 2
                    Layout.fillWidth: true
                    columns: content.width > 700 ? 3 : 1
                    columnSpacing: 14
                    rowSpacing: 14
                    Repeater {
                        model: 3
                        delegate: Rectangle {
                            id: modeCard
                            required property int index
                            readonly property bool current: App.trafficMode === index
                            Layout.fillWidth: true
                            implicitHeight: modeCol.implicitHeight + 32
                            radius: Theme.radiusLarge
                            color: Theme.surface
                            border.width: current ? 2 : 1
                            border.color: current ? Theme.accent : Theme.border
                            ColumnLayout {
                                id: modeCol
                                anchors.fill: parent
                                anchors.margins: 16
                                spacing: 8
                                RowLayout {
                                    Text {
                                        text: page.modeNames[modeCard.index]
                                        color: Theme.text
                                        font.pixelSize: 15
                                        font.weight: Font.DemiBold
                                    }
                                    Item { Layout.fillWidth: true }
                                    Rectangle {
                                        visible: modeCard.current
                                        implicitWidth: activeLbl.implicitWidth + 14
                                        implicitHeight: 20
                                        radius: 10
                                        color: Theme.accentSoft
                                        Text { id: activeLbl; anchors.centerIn: parent; text: qsTr("Active"); color: Theme.accent; font.pixelSize: 11; font.weight: Font.DemiBold }
                                    }
                                }
                                FieldLabel { text: qsTr("Downloads at once") }
                                FSpinBox {
                                    Layout.fillWidth: true
                                    from: 1; to: 50
                                    value: page.tum.length ? page.tum[modeCard.index].maxDownloads : 1
                                    onValueModified: { page.tum[modeCard.index].maxDownloads = value; page.saveTum(modeCard.index) }
                                }
                                FieldLabel { text: qsTr("Connections per download") }
                                FSpinBox {
                                    Layout.fillWidth: true
                                    from: 1; to: 32
                                    value: page.tum.length ? page.tum[modeCard.index].maxConnections : 1
                                    onValueModified: { page.tum[modeCard.index].maxConnections = value; page.saveTum(modeCard.index) }
                                }
                                FieldLabel { text: qsTr("Speed limit (KB/s)") }
                                FSpinBox {
                                    Layout.fillWidth: true
                                    from: 0; to: 1000000; stepSize: 64
                                    zeroText: qsTr("Unlimited")
                                    value: page.tum.length ? page.tum[modeCard.index].speedLimit : 0
                                    onValueModified: { page.tum[modeCard.index].speedLimit = value; page.saveTum(modeCard.index) }
                                }
                            }
                        }
                    }
                }
                Text {
                    visible: page.section === 2
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textDim
                    font.pixelSize: Theme.fontSmall
                    text: qsTr("Snail mode also throttles BitTorrent uploads so your connection stays usable.")
                }

                // ---------------- Snail schedule
                ColumnLayout {
                    visible: page.section === 3
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        Layout.fillWidth: true
                        visible: page.rules.length === 0
                        implicitHeight: 120
                        radius: Theme.radiusLarge
                        color: Theme.surface
                        border.color: Theme.border
                        Column {
                            anchors.centerIn: parent
                            spacing: 8
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: Theme.icons.clock
                                font.family: Theme.iconFont
                                font.pixelSize: 26
                                color: Theme.textDisabled
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("No rules yet — the mode you pick in the status bar is always used.")
                                color: Theme.textDim
                                font.pixelSize: Theme.fontBody
                            }
                        }
                    }

                    Repeater {
                        model: page.rules
                        delegate: Rectangle {
                            id: ruleRow
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            implicitHeight: ruleLayout.implicitHeight + 24
                            radius: Theme.radiusLarge
                            color: Theme.surface
                            border.color: Theme.border
                            opacity: ruleRow.modelData.enabled ? 1 : 0.6
                            RowLayout {
                                id: ruleLayout
                                anchors.fill: parent
                                anchors.leftMargin: 16
                                anchors.rightMargin: 10
                                spacing: 12
                                FToggle {
                                    checked: ruleRow.modelData.enabled
                                    onToggled: { page.rules[ruleRow.index].enabled = checked; page.saveRules() }
                                }
                                ScheduleEditor {
                                    days: ruleRow.modelData.days
                                    fromMin: ruleRow.modelData.from
                                    toMin: ruleRow.modelData.to
                                    onEdited: {
                                        page.rules[ruleRow.index].days = days
                                        page.rules[ruleRow.index].from = fromMin
                                        page.rules[ruleRow.index].to = toMin
                                        page.saveRules()
                                    }
                                }
                                Text { text: Theme.icons.chevronRight; font.family: Theme.iconFont; font.pixelSize: 10; color: Theme.textDim }
                                FComboBox {
                                    Layout.preferredWidth: 120
                                    model: page.modeNames
                                    currentIndex: ruleRow.modelData.mode
                                    onActivated: (i) => { page.rules[ruleRow.index].mode = i; page.saveRules() }
                                }
                                Item { Layout.fillWidth: true }
                                IconButton {
                                    glyph: Theme.icons.del
                                    glyphColor: Theme.textDim
                                    tip: qsTr("Remove rule")
                                    onClicked: page.removeRule(ruleRow.index)
                                }
                            }
                        }
                    }
                    FButton {
                        kind: "secondary"
                        glyph: Theme.icons.add
                        text: qsTr("Add rule")
                        onClicked: page.addRule()
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        wrapMode: Text.WordWrap
                        color: Theme.textDim
                        font.pixelSize: Theme.fontSmall
                        text: qsTr("The first matching rule wins. Picking a mode by hand in the status bar overrides the active rule until its time window ends. A clock icon in the status bar shows when a rule is in control.")
                    }
                }

                // ---------------- BitTorrent
                SettingsCard {
                    visible: page.section === 4
                    title: qsTr("Sharing")
                    Layout.fillWidth: true
                    SettingRow {
                        title: qsTr("Upload limit")
                        description: qsTr("In KB/s.")
                        FSpinBox {
                            id: upField
                            from: 0; to: 1000000; stepSize: 16
                            zeroText: qsTr("Unlimited")
                            onValueModified: App.setUploadLimitKBps(value)
                        }
                    }
                    SettingRow {
                        title: qsTr("Stop seeding at share ratio")
                        description: qsTr("Uploaded ÷ downloaded. 0 stops right after the download completes.")
                        FSpinBox {
                            id: ratioField
                            from: 0; to: 1000; stepSize: 1
                            textFromValue: (v) => (v / 10).toFixed(1)
                            valueFromText: (t) => Math.round(parseFloat(t) * 10)
                            onValueModified: App.setSeedRatio(value / 10)
                        }
                    }
                    SettingRow {
                        title: qsTr("Choose files before downloading")
                        description: qsTr("When a torrent has several files, ask which ones to get before anything is downloaded.")
                        FToggle { checked: App.chooseTorrentFiles; onToggled: App.chooseTorrentFiles = checked }
                    }
                    SettingRow {
                        title: qsTr("Watch a folder for .torrent files")
                        description: App.torrentWatchFolder !== ""
                                     ? qsTr("New .torrent files in %1 are added right away and renamed to .torrent.added.").arg(App.torrentWatchFolder)
                                     : qsTr("Off. Pick a folder, e.g. where your browser saves .torrent files.")
                        RowLayout {
                            spacing: 8
                            FButton { text: App.torrentWatchFolder !== "" ? qsTr("Change…") : qsTr("Choose…"); onClicked: watchDlg.open() }
                            FButton { visible: App.torrentWatchFolder !== ""; kind: "ghost"; text: qsTr("Stop"); onClicked: App.torrentWatchFolder = "" }
                        }
                    }
                    SettingRow {
                        title: qsTr("Listening port")
                        description: qsTr("Other peers connect to this port. Takes effect the next time FluxDM starts.")
                        FSpinBox {
                            implicitWidth: 130
                            from: 1024; to: 65535
                            Component.onCompleted: value = App.btListenPort
                            onValueModified: App.btListenPort = value
                        }
                    }
                    SettingRow {
                        title: qsTr("Open magnet links and .torrent files with FluxDM")
                        description: qsTr("Registers FluxDM for your Windows account only.")
                        divider: false
                        FToggle { id: assocToggle; onToggled: App.setTorrentAssociated(checked) }
                    }
                }

                // ---------------- Remote
                SettingsRemote { visible: page.section === 7; Layout.fillWidth: true }

                // ---------------- Network
                SettingsNetwork { visible: page.section === 6; Layout.fillWidth: true }

                // ---------------- Browser
                SettingsBrowser { id: browserSection; visible: page.section === 5; Layout.fillWidth: true }

                Item { implicitHeight: 24 }
            }
        }
    }

    FileDialog {
        id: ffmpegDlg
        title: qsTr("Locate ffmpeg.exe")
        nameFilters: [qsTr("ffmpeg (ffmpeg.exe)"), qsTr("Programs (*.exe)")]
        onAccepted: {
            ffmpegField.text = App.urlToLocal(selectedFile)
            App.setFfmpegSetting(ffmpegField.text)
            page.ffmpeg = App.ffmpegFound()
        }
    }

    FolderDialog {
        id: categoryDlg
        property string category: ""
        onAccepted: { App.setCategoryFolder(category, App.urlToLocal(selectedFolder)); page.folderRev++ }
    }

    FolderDialog {
        id: folderDlg
        currentFolder: App.toUrl(folderField.text)
        onAccepted: {
            folderField.text = App.urlToLocal(selectedFolder)
            App.defaultFolder = folderField.text
        }
    }

    FolderDialog {
        id: watchDlg
        title: qsTr("Folder to watch for .torrent files")
        onAccepted: App.torrentWatchFolder = App.urlToLocal(selectedFolder)
    }

    FileDialog {
        id: ytdlpDlg
        title: qsTr("Locate yt-dlp.exe")
        nameFilters: [qsTr("yt-dlp (yt-dlp.exe)"), qsTr("Programs (*.exe)")]
        onAccepted: {
            ytdlpField.text = App.urlToLocal(selectedFile)
            App.setYtdlpSetting(ytdlpField.text)
            page.ytdlp = App.ytdlpFound()
        }
    }
}
