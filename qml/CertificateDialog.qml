import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A server's TLS certificate isn't trusted (self-signed, expired, wrong name, ...).
FDialog {
    id: dlg
    property var target: null
    readonly property string host: target ? target.url.replace(/^\w+:\/\//, "").split(/[\/:?#]/)[0] : ""
    title: qsTr("Untrusted certificate")
    subtitle: host
    width: Math.min(520, parent.width - 48)

    function openFor(d) {
        target = d
        open()
    }

    contentItem: ColumnLayout {
        spacing: 10
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: Theme.fontBody
            text: qsTr("The security certificate of %1 isn't trusted, so FluxDM can't be sure it's talking to the real server.").arg(dlg.host)
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.textDim
            font.pixelSize: Theme.fontSmall
            text: dlg.target ? dlg.target.error.replace(/^Untrusted certificate:\s*/, "") : ""
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: Theme.warning
            font.pixelSize: Theme.fontSmall
            text: qsTr("Continue only if you know this server, for example a device on your own network. Otherwise someone could be intercepting the download.")
        }
    }

    buttons: [
        FButton { text: qsTr("Cancel"); onClicked: dlg.close() },
        FButton {
            kind: "primary"
            text: qsTr("Trust this server and continue")
            onClicked: { App.trustCertificate(dlg.target.id); dlg.close() }
        }
    ]
}
