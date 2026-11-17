import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: dlg
    property var target: null
    title: qsTr("Delete download")
    modal: true
    anchors.centerIn: parent
    width: Math.min(480, parent.width - 40)
    padding: 20

    function openFor(d) {
        target = d
        open()
    }

    background: Rectangle { color: Theme.surface; radius: 10; border.color: Theme.border }
    header: Label { text: dlg.title; color: Theme.text; font.pixelSize: 17; padding: 20; bottomPadding: 0 }

    contentItem: Text {
        wrapMode: Text.WordWrap
        color: Theme.text
        font.pixelSize: 13
        text: dlg.target
              ? qsTr("Delete \"%1\"?").arg(dlg.target.title)
                + (dlg.target.completed ? "" : "\n\n" + qsTr("The download is not finished; its partial data will be removed."))
              : ""
    }

    footer: Item {
        implicitHeight: 64
        RowLayout {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Button { text: qsTr("Cancel"); onClicked: dlg.close() }
            Button {
                text: qsTr("Remove from list")
                onClicked: { App.remove(dlg.target.id, false); dlg.close() }
            }
            Button {
                text: qsTr("Delete file")
                highlighted: true
                visible: dlg.target !== null && dlg.target.completed
                onClicked: { App.remove(dlg.target.id, true); dlg.close() }
            }
        }
    }
}
