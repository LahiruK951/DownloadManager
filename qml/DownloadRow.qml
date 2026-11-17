import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: row
    required property var download
    required property int index
    signal requestDelete(var download)
    signal requestSchedule(var download)

    readonly property bool isSelected: App.selected === download
    readonly property var d: download

    height: App.compactView ? 40 : 66
    color: isSelected ? Theme.selection : (ma.containsMouse ? Theme.hover : "transparent")

    function progressText() {
        if (d.total > 0)
            return App.fmtSize(d.received) + " " + qsTr("of") + " " + App.fmtSize(d.total)
                   + " (" + Math.floor(d.progress * 100) + "%)"
        return App.fmtSize(d.received)
    }

    function statusLine() {
        if (d.completed && d.seeding)
            return qsTr("Seeding") + " · ↑ " + App.fmtSpeed(d.uploadSpeed) + " · " + qsTr("ratio") + " " + d.ratio.toFixed(2)
                   + " · " + d.peers + " " + qsTr("peers")
        if (d.completed) return qsTr("Done") + " · " + App.fmtDate(d.finishedAt)
        if (d.failed) return qsTr("Error") + ": " + d.error
        if (d.paused) return qsTr("Paused") + " · " + progressText()
        if (d.waitingForSchedule) return qsTr("Scheduled") + " · " + d.scheduleText + " · " + progressText()
        if (d.queued) return qsTr("Queued") + " · " + progressText()
        if (d.starting) return d.isTorrent ? qsTr("Fetching torrent information…") : qsTr("Connecting…")
        let s = progressText()
        if (d.eta >= 0) s += " · " + App.fmtEta(d.eta) + " " + qsTr("left")
        if (d.isTorrent)
            s += " · " + d.seeds + " " + qsTr("seeds") + ", " + d.peers + " " + qsTr("peers")
        else if (d.connections > 0)
            s += " · " + d.connections + " " + (d.connections === 1 ? qsTr("connection") : qsTr("connections"))
        if (d.scheduled) s += " · " + d.scheduleText
        return s
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton) {
                App.select(row.d.id)
                ctx.popup()
                return
            }
            if (row.isSelected)
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

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 10
        spacing: 12

        Rectangle {
            readonly property color c: Theme.categoryColor(row.d.category)
            Layout.preferredWidth: App.compactView ? 26 : 40
            Layout.preferredHeight: Layout.preferredWidth
            radius: App.compactView ? 6 : 10
            color: Qt.rgba(c.r, c.g, c.b, Theme.dark ? 0.22 : 0.13)
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

            Text {
                Layout.fillWidth: true
                text: row.d.title
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: 14
            }
            ProgressTrack {
                Layout.fillWidth: true
                visible: !App.compactView && !row.d.completed
                value: row.d.progress
                active: row.d.running
                failed: row.d.failed
            }
            Text {
                Layout.fillWidth: true
                visible: !App.compactView
                text: row.statusLine()
                elide: Text.ElideRight
                color: row.d.failed ? Theme.error : (row.d.completed ? Theme.success : Theme.textDim)
                font.pixelSize: 12
            }
        }

        Text {
            Layout.preferredWidth: 96
            horizontalAlignment: Text.AlignRight
            text: row.d.running && row.d.speed > 0 ? App.fmtSpeed(row.d.speed)
                : (row.d.seeding && row.d.uploadSpeed > 0 ? "↑ " + App.fmtSpeed(row.d.uploadSpeed) : "")
            color: Theme.accent
            font.pixelSize: 13
        }
        Text {
            Layout.preferredWidth: 80
            horizontalAlignment: Text.AlignRight
            text: row.d.total >= 0 ? App.fmtSize(row.d.total) : ""
            color: Theme.textDim
            font.pixelSize: 13
        }
        IconButton {
            glyph: (row.d.completed && row.d.seeding) ? Theme.icons.upload
                 : row.d.completed ? Theme.icons.open
                 : (row.d.running || row.d.queued) ? Theme.icons.pause
                 : row.d.failed ? Theme.icons.refresh : Theme.icons.play
            tip: (row.d.completed && row.d.seeding) ? qsTr("Stop seeding")
                 : row.d.completed ? qsTr("Open") : (row.d.running || row.d.queued) ? qsTr("Pause") : qsTr("Resume")
            opacity: ma.containsMouse || hovered || row.isSelected ? 1 : 0.55
            onClicked: (row.d.completed && !row.d.seeding) ? App.openFile(row.d.id) : App.togglePause(row.d.id)
        }
        IconButton {
            glyph: Theme.icons.folder
            tip: qsTr("Show in folder")
            opacity: ma.containsMouse || hovered || row.isSelected ? 1 : 0.55
            onClicked: App.openFolder(row.d.id)
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 14
        height: 1
        color: Theme.border
        opacity: 0.6
    }

    FMenu {
        id: ctx
        FMenuItem { glyph: Theme.icons.open; text: qsTr("Open"); enabled: row.d.completed; onTriggered: App.openFile(row.d.id) }
        FMenuItem { glyph: Theme.icons.folder; text: qsTr("Show in folder"); onTriggered: App.openFolder(row.d.id) }
        FMenuSeparator {}
        FMenuItem {
            text: (row.d.running || row.d.queued) ? qsTr("Pause") : qsTr("Resume")
            enabled: !row.d.completed
            onTriggered: App.togglePause(row.d.id)
        }
        FMenuItem {
            text: qsTr("Stop seeding")
            visible: row.d.seeding
            height: visible ? implicitHeight : 0
            onTriggered: App.stopSeeding(row.d.id)
        }
        FMenuItem {
            text: row.d.scheduled ? qsTr("Edit schedule…") : qsTr("Schedule…")
            enabled: !row.d.completed
            onTriggered: row.requestSchedule(row.d)
        }
        FMenuItem { glyph: Theme.icons.refresh; text: qsTr("Restart"); onTriggered: App.restart(row.d.id) }
        FMenuItem { glyph: Theme.icons.copy; text: qsTr("Copy link"); onTriggered: App.copyLink(row.d.id) }
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
