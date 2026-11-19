import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Create or edit a user tag: name, colour, file types that get it automatically, and an
// optional folder for those downloads (FDM: tags + downloadsTagsDetails).
FDialog {
    id: dlg
    property int tagId: 0
    property string color: palette[5]
    property string error: ""
    property var assignTo: []  // download ids to tag right after creating it
    readonly property var palette: ["#e5534b", "#f0883e", "#c69026", "#57ab5a", "#2ea8b8", "#539bf5", "#986ee2", "#e275ad", "#768390"]
    title: tagId > 0 ? qsTr("Edit tag") : qsTr("New tag")
    width: Math.min(500, parent.width - 48)

    function openNew(downloadIds) {
        tagId = 0
        nameField.text = ""
        extField.text = ""
        folderField.text = ""
        color = palette[(App.tags.length + 5) % palette.length]
        assignTo = downloadIds || []
        error = ""
        open()
        nameField.forceActiveFocus()
    }
    function openEdit(t) {
        tagId = t.id
        nameField.text = t.name
        extField.text = t.extensions
        folderField.text = t.folder
        color = t.color
        assignTo = []
        error = ""
        open()
        nameField.forceActiveFocus()
    }
    function apply() {
        if (App.tagNameTaken(nameField.text, tagId)) {
            error = qsTr("There is already a tag with this name.")
            return
        }
        const id = App.saveTag(tagId, nameField.text, color, extField.text, folderField.text)
        if (id <= 0) {
            error = qsTr("Enter a name.")
            return
        }
        for (const d of assignTo) App.setTag(d, id, true)
        close()
    }

    contentItem: ColumnLayout {
        spacing: 6
        FieldLabel { text: qsTr("Name") }
        FTextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("e.g. Work, Linux ISOs, Podcasts")
            onTextEdited: dlg.error = ""
            onAccepted: dlg.apply()
        }
        FieldLabel { text: qsTr("Colour"); Layout.topMargin: 6 }
        Row {
            spacing: 8
            Repeater {
                model: dlg.palette
                delegate: Rectangle {
                    required property string modelData
                    width: 24; height: 24; radius: 12
                    color: modelData
                    border.width: dlg.color === modelData ? 3 : 0
                    border.color: Theme.surface
                    Rectangle { // ring around the chosen colour
                        anchors.centerIn: parent
                        width: 30; height: 30; radius: 15
                        color: "transparent"
                        border.width: 2
                        border.color: parent.color
                        visible: dlg.color === parent.modelData
                    }
                    TapHandler { onTapped: dlg.color = parent.modelData }
                }
            }
        }
        FieldLabel { text: qsTr("Add automatically to these file types (optional)"); Layout.topMargin: 6 }
        FTextField {
            id: extField
            Layout.fillWidth: true
            placeholderText: qsTr("e.g. iso, img")
        }
        FieldLabel { text: qsTr("Save those downloads to (optional)"); Layout.topMargin: 6 }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            FTextField {
                id: folderField
                Layout.fillWidth: true
                placeholderText: qsTr("The usual folder")
            }
            FButton { text: qsTr("Browse…"); onClicked: folderDlg.open() }
        }
        Text {
            Layout.fillWidth: true
            visible: dlg.error !== ""
            text: dlg.error
            color: Theme.error
            font.pixelSize: Theme.fontSmall
        }
    }

    FolderDialog {
        id: folderDlg
        onAccepted: folderField.text = App.urlToLocal(selectedFolder)
    }

    buttons: [
        FButton {
            visible: dlg.tagId > 0
            kind: "ghost"
            text: qsTr("Delete tag")
            onClicked: { App.removeTag(dlg.tagId); dlg.close() }
        },
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            text: dlg.tagId > 0 ? qsTr("Save") : qsTr("Create")
            enabled: nameField.text.trim() !== ""
            onClicked: dlg.apply()
        }
    ]
}
