import QtQuick

// Visualises which byte ranges are already on disk (one block per connection's progress).
Rectangle {
    id: bar
    property var segments: []

    implicitHeight: 12
    radius: 3
    color: Theme.track
    clip: true

    Repeater {
        model: bar.segments
        delegate: Rectangle {
            required property var modelData
            x: modelData[0] * bar.width
            width: Math.max(1, (modelData[1] - modelData[0]) * bar.width)
            height: bar.height
            color: Theme.accent
        }
    }
}
