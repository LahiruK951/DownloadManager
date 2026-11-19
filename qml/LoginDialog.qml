import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Server login for a download that failed with "login required" (HTTP 401, FTP 530).
FDialog {
    id: dlg
    property var target: null
    property bool failedBefore: false // the last attempt was rejected, not just missing
    title: qsTr("Sign in")
    subtitle: target ? App.loginServer(target.id) : ""
    width: Math.min(460, parent.width - 48)

    function openFor(d) {
        target = d
        failedBefore = d.error.startsWith("Login failed")
        userField.text = App.loginUser(d.id)
        passField.text = ""
        rememberBox.checked = true
        open()
        if (userField.text === "") userField.forceActiveFocus()
        else passField.forceActiveFocus()
    }

    function apply() {
        if (userField.text.trim() === "") return
        App.signIn(target.id, userField.text, passField.text, rememberBox.checked)
        dlg.close()
    }

    contentItem: ColumnLayout {
        spacing: 10
        Text {
            Layout.fillWidth: true
            text: dlg.failedBefore ? qsTr("The server didn't accept that user name or password. Try again.")
                                   : qsTr("This server needs a user name and password to download %1.").arg(dlg.target ? dlg.target.title : "")
            wrapMode: Text.Wrap
            color: dlg.failedBefore ? Theme.error : Theme.textDim
            font.pixelSize: Theme.fontBody
        }
        FieldLabel { text: qsTr("User name") }
        FTextField {
            id: userField
            Layout.fillWidth: true
            onAccepted: passField.forceActiveFocus()
        }
        FieldLabel { text: qsTr("Password") }
        FTextField {
            id: passField
            Layout.fillWidth: true
            echoMode: TextInput.Password
            onAccepted: dlg.apply()
        }
        FCheckBox {
            id: rememberBox
            text: qsTr("Remember for this server")
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            text: qsTr("Sign in")
            enabled: userField.text.trim() !== ""
            onClicked: dlg.apply()
        }
    ]
}
