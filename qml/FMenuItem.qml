import QtQuick
import QtQuick.Controls

MenuItem {
    id: mi
    property string glyph: ""
    property bool danger: false

    implicitHeight: visible ? 32 : 0
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: Theme.fontBody
    hoverEnabled: true

    // left slot: check mark for checkable items, otherwise an optional icon
    indicator: Text {
        x: mi.leftPadding
        width: 18
        anchors.verticalCenter: parent.verticalCenter
        horizontalAlignment: Text.AlignHCenter
        text: mi.checkable ? (mi.checked ? Theme.icons.check : "") : mi.glyph
        font.family: Theme.iconFont
        font.pixelSize: mi.checkable ? 11 : 13
        color: mi.checkable ? Theme.accent : (mi.danger ? Theme.error : Theme.textDim)
    }

    arrow: Text {
        x: mi.width - width - mi.rightPadding
        anchors.verticalCenter: parent.verticalCenter
        visible: mi.subMenu
        text: Theme.icons.chevronRight
        font.family: Theme.iconFont
        font.pixelSize: 9
        color: Theme.textDim
    }

    contentItem: Text {
        leftPadding: 28
        rightPadding: mi.subMenu ? 16 : 0
        text: mi.text
        font: mi.font
        color: !mi.enabled ? Theme.textDisabled : (mi.danger ? Theme.error : Theme.text)
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: 6
        color: mi.highlighted && mi.enabled ? (mi.danger ? Theme.errorSoft : Theme.hover) : "transparent"
    }
}
