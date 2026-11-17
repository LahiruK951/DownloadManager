import QtQuick
import QtQuick.Controls

Item {
    id: root
    signal requestDelete(var download)
    signal requestSchedule(var download)

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
        }

        focus: true
        Keys.onUpPressed: move(-1)
        Keys.onDownPressed: move(1)
        Keys.onSpacePressed: if (App.selected) App.togglePause(App.selected.id)
        Keys.onReturnPressed: if (App.selected && App.selected.completed) App.openFile(App.selected.id)

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

    Column {
        anchors.centerIn: parent
        visible: list.count === 0
        spacing: 10
        width: Math.min(parent.width - 40, 420)

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Theme.icons.down
            font.family: Theme.iconFont
            font.pixelSize: 44
            color: Theme.textDisabled
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: App.search !== "" ? qsTr("Nothing matches your search")
                 : App.filter === "all" ? qsTr("No downloads yet") : qsTr("Nothing here")
            color: Theme.text
            font.pixelSize: 16
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            visible: App.filter === "all" && App.search === ""
            text: qsTr("Click + or press Ctrl+V to paste a link. You can also drag links onto this window.")
            color: Theme.textDim
            font.pixelSize: 13
        }
    }
}
