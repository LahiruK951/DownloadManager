import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    signal requestDelete(var download)
    signal requestSchedule(var download)
    signal requestAdd()
    signal requestDeleteSelection()
    signal requestSpeedLimit(var download)
    signal requestMove()
    signal requestRename(var download)
    signal requestLogin(var download)
    signal requestExisting(var download)
    signal requestFiles(var download)

    // Developer aid (screenshots): pop up the context menu of row `i`.
    function showContextMenu(i) {
        list.positionViewAtIndex(i, ListView.Contain)
        const it = list.itemAtIndex(i)
        if (it) it.showMenu()
    }
    signal requestOpenTorrent()

    ListView {
        id: list
        anchors.fill: parent
        clip: true
        model: App.downloads
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        delegate: DownloadRow {
            width: list.width
            onRequestDelete: (d) => root.requestDelete(d)
            onRequestSchedule: (d) => root.requestSchedule(d)
            onRequestDeleteSelection: root.requestDeleteSelection()
            onRequestSpeedLimit: (d) => root.requestSpeedLimit(d)
            onRequestMove: root.requestMove()
            onRequestRename: (d) => root.requestRename(d)
            onRequestLogin: (d) => root.requestLogin(d)
            onRequestExisting: (d) => root.requestExisting(d)
            onRequestFiles: (d) => root.requestFiles(d)
        }

        focus: true
        Keys.onUpPressed: move(-1)
        Keys.onDownPressed: move(1)
        Keys.onSpacePressed: if (App.selected) App.togglePause(App.selected.id)
        Keys.onReturnPressed: if (App.selected) { if (App.selectionCount > 1) App.openSelected(); else App.openFile(App.selected.id) }
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_F2 && App.selected && App.selectionCount === 1 && App.canRename(App.selected.id)) {
                root.requestRename(App.selected)
                event.accepted = true
            }
        }

        function move(delta) {
            const n = list.count
            if (n === 0) return
            let cur = -1
            for (let i = 0; i < n; ++i)
                if (App.selected && App.downloads.idAt(i) === App.selected.id) { cur = i; break }
            const next = Math.max(0, Math.min(n - 1, cur + delta))
            App.select(App.downloads.idAt(next))
            list.positionViewAtIndex(next, ListView.Contain)
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        visible: list.count === 0
        spacing: 10
        width: Math.min(parent.width - 40, 440)

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: 6
            implicitWidth: 76
            implicitHeight: 76
            radius: 22
            color: Theme.accentSoft
            Text {
                anchors.centerIn: parent
                text: App.search !== "" ? Theme.icons.search : Theme.icons.down
                font.family: Theme.iconFont
                font.pixelSize: 32
                color: Theme.accent
            }
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: App.search !== "" ? qsTr("Nothing matches “%1”").arg(App.search)
                 : App.filter === "all" ? qsTr("No downloads yet") : qsTr("Nothing here")
            color: Theme.text
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: App.search !== "" ? qsTr("Try another name or part of the link.")
                 : App.filter === "all" ? qsTr("Paste a link with Ctrl+V, drop links onto this window, or add a magnet link or .torrent file.")
                 : qsTr("No downloads in this category.")
            color: Theme.textDim
            font.pixelSize: Theme.fontBody
        }
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            spacing: 8
            visible: App.filter === "all" && App.search === ""
            FButton { kind: "primary"; glyph: Theme.icons.add; text: qsTr("Add download"); onClicked: root.requestAdd() }
            FButton { visible: App.torrentSupport; glyph: Theme.icons.link; text: qsTr("Open torrent file…"); onClicked: root.requestOpenTorrent() }
        }
        FButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 8
            visible: App.filter !== "all" || App.search !== ""
            text: qsTr("Show all downloads")
            onClicked: { App.search = ""; App.filter = "all" }
        }
    }
}