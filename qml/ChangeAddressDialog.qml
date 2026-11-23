import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A new link for an unfinished download whose link expired (FDM/IDM: "Change URL" / "Refresh
// address"). The part already downloaded is kept and continued if the new link is the same file.
FDialog {
    id: dlg
    property var target: null
    property string error: ""
    title: qsTr("Change address")
    subtitle: target ? target.title : ""
    width: Math.min(600, parent.width - 48)

    function openFor(d) {
        target = d
        linkField.text = d.url
        pageField.text = App.hasPage(d.id) ? d.referrer : ""
        error = ""
        open()
        linkField.forceActiveFocus()
        linkField.selectAll()
    }

    function save() {
        dlg.error = App.changeAddress(dlg.target.id, linkField.text, pageField.text)
        if (dlg.error === "") dlg.close()
    }

    contentItem: ColumnLayout {
        spacing: 6
        Text {
            Layout.fillWidth: true
            Layout.bottomMargin: 6
            wrapMode: Text.Wrap
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
            text: dlg.target && dlg.target.received > 0
                  ? qsTr("Open the download page, start the download again (or copy its link) and paste the new link here. FluxDM keeps the %1 already downloaded and continues if it is the same file.").arg(App.fmtSize(dlg.target.received))
                  : qsTr("Open the download page, start the download again (or copy its link) and paste the new link here.")
        }

        FieldLabel { text: qsTr("Link") }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            FTextField {
                id: linkField
                Layout.fillWidth: true
                placeholderText: "https://example.com/file.zip"
                onAccepted: dlg.save()
            }
            FButton {
                text: qsTr("Paste")
                onClicked: {
                    const l = App.clipboardLink()
                    if (l !== "") linkField.text = l
                    else dlg.error = qsTr("The clipboard has no link.")
                }
            }
        }

        FieldLabel { text: qsTr("Download page (optional)"); Layout.topMargin: 8 }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            FTextField {
                id: pageField
                Layout.fillWidth: true
                placeholderText: dlg.target ? App.pageUrl(dlg.target.id) : ""
                onAccepted: dlg.save()
            }
            FButton {
                glyph: Theme.icons.globe
                text: pageField.text !== "" ? qsTr("Open page") : qsTr("Open website")
                enabled: pageField.text !== "" || (dlg.target !== null && App.pageUrl(dlg.target.id) !== "")
                onClicked: App.openUrl(pageField.text !== "" ? pageField.text : App.pageUrl(dlg.target.id))
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: dlg.error !== ""
            text: dlg.error
            wrapMode: Text.Wrap
            color: Theme.error
            font.pixelSize: Theme.fontSmall
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton { kind: "primary"; text: qsTr("Save and continue"); onClicked: dlg.save() }
    ]
}
