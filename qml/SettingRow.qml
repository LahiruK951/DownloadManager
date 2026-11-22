import QtQuick
import QtQuick.Layouts

// One row in a SettingsCard: title/description on the left, a control on the right.
Item {
    id: r
    property string title: ""
    property string description: ""
    property bool divider: true
    default property alias control: slot.data

    Layout.fillWidth: true
    implicitHeight: Math.max(56, texts.implicitHeight + 24, slot.childrenRect.height + 20)

    ColumnLayout {
        id: texts
        anchors.left: parent.left
        anchors.right: slot.left
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2
        Text {
            Layout.fillWidth: true
            text: r.title
            color: Theme.text
            font.pixelSize: Theme.fontBody
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: r.description !== ""
            text: r.description
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }
    }
    Item {
        id: slot
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: childrenRect.width
        height: childrenRect.height
    }
    // Screen readers: a bare control (switch, number, list) is announced by this row's title.
    Component.onCompleted: {
        for (let i = 0; i < slot.children.length; ++i) {
            const c = slot.children[i]
            if (c.Accessible && !c.Accessible.name && !c.text) {
                c.Accessible.name = r.title
                c.Accessible.description = r.description
            }
        }
    }
    Rectangle {
        visible: r.divider
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
        opacity: 0.7
    }
}
