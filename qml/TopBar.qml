import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: bar
    signal addClicked()
    signal deleteClicked()
    signal settingsClicked()
    signal aboutClicked()
    signal openTorrentClicked()

    function focusSearch() { search.forceActiveFocus(); search.selectAll() }
    function openMenu() { mainMenu.popup(menuBtn, 0, menuBtn.height) }

    implicitHeight: 56
    color: Theme.surface

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 10
        spacing: 4

        AbstractButton {
            id: addBtn
            implicitWidth: 40
            implicitHeight: 40
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            onClicked: bar.addClicked()
            background: Rectangle {
                radius: 20
                color: addBtn.down ? Theme.accentHover : (addBtn.hovered ? Theme.accentHover : Theme.accent)
            }
            contentItem: Text {
                text: Theme.icons.add
                font.family: Theme.iconFont
                font.pixelSize: 16
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Add download (Ctrl+N)")
            ToolTip.delay: 500
        }

        Item { implicitWidth: 10 }

        IconButton { glyph: Theme.icons.play; tip: qsTr("Resume all"); onClicked: App.resumeAll() }
        IconButton { glyph: Theme.icons.pause; tip: qsTr("Pause all"); onClicked: App.pauseAll() }
        IconButton {
            glyph: Theme.icons.del
            tip: qsTr("Delete selected (Del)")
            enabled: App.selected !== null
            onClicked: bar.deleteClicked()
        }

        Item { Layout.fillWidth: true }

        TextField {
            id: search
            Layout.preferredWidth: 280
            implicitHeight: 34
            placeholderText: qsTr("Search downloads")
            leftPadding: 32
            text: App.search
            onTextEdited: App.search = text
            color: Theme.text
            placeholderTextColor: Theme.textDim
            background: Rectangle {
                radius: 17
                color: Theme.surfaceAlt
                border.width: search.activeFocus ? 1 : 0
                border.color: Theme.accent
            }
            Text {
                text: Theme.icons.search
                font.family: Theme.iconFont
                font.pixelSize: 13
                color: Theme.textDim
                anchors.left: parent.left
                anchors.leftMargin: 11
                anchors.verticalCenter: parent.verticalCenter
            }
            Keys.onEscapePressed: { text = ""; App.search = "" }
        }

        IconButton {
            id: menuBtn
            glyph: Theme.icons.menu
            tip: qsTr("Menu")
            onClicked: mainMenu.popup(menuBtn, 0, menuBtn.height)

            FMenu {
                id: mainMenu
                FMenuItem { glyph: Theme.icons.add; text: qsTr("Add download…"); onTriggered: bar.addClicked() }
                FMenuItem { glyph: Theme.icons.link; text: qsTr("Open torrent file…"); visible: App.torrentSupport; height: visible ? implicitHeight : 0; onTriggered: bar.openTorrentClicked() }
                FMenuSeparator {}
                FMenu {
                    title: qsTr("When downloads finish")
                    Repeater {
                        model: [qsTr("Do nothing"), qsTr("Sleep"), qsTr("Hibernate"), qsTr("Shut down"), qsTr("Exit FluxDM")]
                        FMenuItem {
                            required property string modelData
                            required property int index
                            text: modelData
                            checkable: true
                            checked: App.whenDone === index
                            onTriggered: App.whenDone = index
                        }
                    }
                }
                FMenuItem {
                    text: qsTr("Compact view")
                    checkable: true
                    checked: App.compactView
                    onTriggered: App.compactView = checked
                }
                FMenu {
                    title: qsTr("Theme")
                    FMenuItem { text: qsTr("System"); checkable: true; checked: App.theme === "system"; onTriggered: App.theme = "system" }
                    FMenuItem { text: qsTr("Light"); checkable: true; checked: App.theme === "light"; onTriggered: App.theme = "light" }
                    FMenuItem { text: qsTr("Dark"); checkable: true; checked: App.theme === "dark"; onTriggered: App.theme = "dark" }
                }
                FMenuItem { glyph: Theme.icons.settings; text: qsTr("Settings…"); onTriggered: bar.settingsClicked() }
                FMenuSeparator {}
                FMenuItem { glyph: Theme.icons.info; text: qsTr("About FluxDM"); onTriggered: bar.aboutClicked() }
                FMenuItem { glyph: Theme.icons.power; text: qsTr("Exit"); onTriggered: App.quit() }
            }
        }
    }

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
    }
}
