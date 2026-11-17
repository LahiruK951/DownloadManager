import QtQuick

// Thin progress bar; value < 0 = indeterminate (size unknown).
Rectangle {
    id: track
    property real value: 0
    property bool active: false
    property bool failed: false

    implicitHeight: 4
    radius: 2
    color: Theme.track
    clip: true

    readonly property color barColor: failed ? Theme.error : (active ? Theme.accent : Theme.textDim)

    Rectangle {
        visible: track.value >= 0
        width: track.width * Math.max(0, Math.min(1, track.value))
        height: parent.height
        radius: 2
        color: track.barColor
        Behavior on width { NumberAnimation { duration: 250 } }
    }

    Rectangle {
        id: runner
        visible: track.value < 0 && track.active
        width: track.width * 0.25
        height: parent.height
        radius: 2
        color: track.barColor
        NumberAnimation on x {
            running: runner.visible
            from: -runner.width
            to: track.width
            duration: 1200
            loops: Animation.Infinite
        }
    }
}
