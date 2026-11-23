import QtQuick
import QtQuick.Controls

ComboBox {
    id: cb
    implicitHeight: 36
    font.pixelSize: Theme.fontBody
    hoverEnabled: true

    // One fixed width that fits the longest choice, whatever is selected (a list of choices), so the
    // box doesn't jump around and the open list never cuts its entries short. An editable box (e.g. a
    // folder) takes the width its layout gives it.
    property real minWidth: 120
    property real maxWidth: 440
    readonly property real widestText: {
        let w = 0
        const n = cb.count
        cb.font.pixelSize // re-measure on zoom
        for (let i = 0; i < n; ++i)
            w = Math.max(w, fm.advanceWidth(cb.textAt(i)))
        return Math.ceil(w)
    }
    implicitWidth: cb.editable ? 200
                 : Math.min(maxWidth, Math.max(minWidth, widestText + 12 + indicator.width + 8 + leftPadding + rightPadding + 4))
    FontMetrics { id: fm; font: cb.font }

    background: Rectangle {
        radius: Theme.radius
        color: Theme.inputBg
        border.width: (cb.activeFocus && cb.editable) || cb.popup.visible ? 2 : 1
        border.color: (cb.activeFocus && cb.editable) || cb.popup.visible ? Theme.accent
                    : (cb.hovered ? Theme.borderStrong : Theme.border)
    }

    contentItem: TextField {
        leftPadding: 12
        rightPadding: cb.indicator.width + 8
        text: cb.editable ? cb.editText : cb.displayText
        enabled: cb.editable
        autoScroll: cb.editable
        readOnly: cb.down
        font: cb.font
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: "white"
        selectByMouse: true
        verticalAlignment: Text.AlignVCenter
        background: null
    }

    indicator: Text {
        x: cb.width - width - 12
        y: (cb.height - height) / 2
        text: Theme.icons.chevronDown
        font.family: Theme.iconFont
        font.pixelSize: 10
        color: Theme.textDim
    }

    delegate: ItemDelegate {
        id: item
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : cb.width
        implicitHeight: 32
        highlighted: cb.highlightedIndex === index
        contentItem: Text {
            text: item.modelData
            font: cb.font
            color: Theme.text
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 6
            color: item.highlighted ? Theme.hover : "transparent"
            Rectangle {
                visible: cb.currentIndex === item.index
                width: 3
                height: 14
                radius: 2
                x: 0
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.accent
            }
        }
    }

    popup: Popup {
        y: cb.height + 4
        width: Math.max(cb.width, Math.min(cb.maxWidth + 40, cb.widestText + 40))
        padding: 4
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 320)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: cb.popup.visible ? cb.delegateModel : null
            currentIndex: cb.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: Rectangle {
            radius: Theme.radius
            color: Theme.surface
            border.color: Theme.border
        }
    }
}
