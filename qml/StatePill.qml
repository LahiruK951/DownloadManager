import QtQuick

// Small rounded status label: Done, Paused, Scheduled, Error, Seeding...
Rectangle {
    id: pill
    property string text
    property string glyph: ""
    property color fg: Theme.textDim
    property color bg: Theme.surfaceAlt

    implicitWidth: row.implicitWidth + 14
    implicitHeight: 20
    radius: 10
    color: bg

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 4
        Text {
            visible: pill.glyph !== ""
            text: pill.glyph
            font.family: Theme.iconFont
            font.pixelSize: 9
            color: pill.fg
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: pill.text
            font.pixelSize: 11
            font.weight: Font.DemiBold
            color: pill.fg
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
