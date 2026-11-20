import QtQuick
import QtQuick.Window

// One download's details in a window of its own (FDM: standalone download windows), so several
// can stay open next to the list.
Window {
    id: win
    required property var download
    width: 820
    height: 540
    minimumWidth: 520
    minimumHeight: 320
    color: Theme.bg
    title: download ? download.title + " — FluxDM" : "FluxDM"
    visible: true

    onDownloadChanged: if (!download) win.close() // deleted from the list
    onClosing: (close) => win.destroy()

    BottomPanel {
        anchors.fill: parent
        download: win.download
        standalone: true
        onCloseRequested: win.close()
    }
}
