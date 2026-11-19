import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Choose which files of a torrent to download (shown once its file list is known).
FDialog {
    id: dlg
    property var target: null
    property var files: []        // [{index, name, size}]
    property var wanted: ({})     // index -> bool
    property int rev: 0           // bump to refresh bindings that read `wanted`
    title: qsTr("Choose files")
    subtitle: target ? target.title : ""
    width: Math.min(640, parent.width - 48)

    readonly property var totals: {
        rev
        let n = 0, bytes = 0, all = 0
        for (const f of files) {
            all += f.size
            if (wanted[f.index]) { ++n; bytes += f.size }
        }
        return { count: n, bytes: bytes, all: all }
    }

    function openFor(d) {
        target = d
        files = d.pendingFiles
        const w = {}
        for (const f of files) w[f.index] = true
        wanted = w
        rev++
        open()
    }
    function setAll(on) {
        const w = {}
        for (const f of files) w[f.index] = on
        wanted = w
        rev++
    }
    function toggle(i, on) {
        wanted[i] = on
        rev++
    }
    function start() {
        const list = []
        for (const f of files) if (wanted[f.index]) list.push(f.index)
        App.chooseFiles(target.id, list)
        close()
    }

    contentItem: ColumnLayout {
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            FButton { kind: "ghost"; text: qsTr("Select all"); onClicked: dlg.setAll(true) }
            FButton { kind: "ghost"; text: qsTr("Select none"); onClicked: dlg.setAll(false) }
            Item { Layout.fillWidth: true }
            Text {
                text: qsTr("%1 of %2 files · %3 of %4").arg(dlg.totals.count).arg(dlg.files.length)
                          .arg(App.fmtSize(dlg.totals.bytes)).arg(App.fmtSize(dlg.totals.all))
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(320, list.contentHeight + 2)
            radius: Theme.radius
            color: "transparent"
            border.color: Theme.border
            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: dlg.files
                ScrollBar.vertical: ScrollBar {}
                delegate: Item {
                    id: fileRow
                    required property var modelData
                    readonly property bool on: (dlg.rev, dlg.wanted[modelData.index] === true)
                    width: list.width
                    height: 36
                    Rectangle { anchors.fill: parent; color: hoverH.hovered ? Theme.hover : "transparent" }
                    HoverHandler { id: hoverH }
                    TapHandler { onTapped: dlg.toggle(fileRow.modelData.index, !fileRow.on) }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 14
                        spacing: 10
                        FCheckBox {
                            padding: 0
                            checked: fileRow.on
                            onToggled: dlg.toggle(fileRow.modelData.index, checked)
                        }
                        Text {
                            Layout.fillWidth: true
                            text: fileRow.modelData.name
                            elide: Text.ElideMiddle
                            color: fileRow.on ? Theme.text : Theme.textDisabled
                            font.pixelSize: Theme.fontBody
                        }
                        Text {
                            text: App.fmtSize(fileRow.modelData.size)
                            color: Theme.textDim
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                }
            }
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("You can change this later in the Files tab.")
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
        }
    }

    buttons: [
        FButton { text: qsTr("Later"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            glyph: Theme.icons.down
            text: qsTr("Download")
            enabled: dlg.totals.count > 0
            onClicked: dlg.start()
        }
    ]
}
