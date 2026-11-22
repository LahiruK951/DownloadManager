import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Speed graph (total download speed, last 2 minutes) and traffic totals; opened from the status bar.
Popup {
    id: pop
    width: 380
    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    property var usage: ({})
    onOpened: usage = App.usage()
    Timer { interval: 2000; repeat: true; running: pop.visible; onTriggered: pop.usage = App.usage() }

    background: Rectangle {
        color: Theme.surface
        radius: Theme.radius
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("Download speed")
                color: Theme.text
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }
            Item { Layout.fillWidth: true }
            Text {
                text: qsTr("last 2 minutes")
                color: Theme.textDim
                font.pixelSize: Theme.fontSmall
            }
        }

        Item {
            id: plot
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            readonly property var samples: pop.visible ? App.speedHistory : []
            readonly property real peak: Math.max(1, ...samples)
            // A round top for the scale in the units it's shown in (KB/s, MB/s: 1024-based),
            // e.g. a 23 MB/s peak -> 50 MB/s.
            readonly property real scaleMax: {
                const unit = Math.pow(1024, Math.max(0, Math.floor(Math.log(peak) / Math.log(1024))))
                const v = peak / unit
                for (const s of [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000]) if (v <= s) return s * unit
                return 1024 * unit
            }
            property int hover: -1
            onSamplesChanged: chart.requestPaint()

            // Recessive scale: top value and a baseline.
            Text {
                anchors.left: parent.left
                anchors.top: parent.top
                text: App.fmtSpeed(plot.scaleMax)
                color: Theme.textDim
                font.pixelSize: 10
            }
            Rectangle { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 1; color: Theme.border }

            Canvas {
                id: chart
                anchors.fill: parent
                anchors.topMargin: 16
                renderStrategy: Canvas.Cooperative
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    const n = plot.samples.length
                    if (n < 2) return
                    const step = width / 119 // 120 slots: the newest sample sits at the right edge
                    const x0 = width - (n - 1) * step
                    const yOf = (v) => height - (v / plot.scaleMax) * height
                    ctx.beginPath()
                    ctx.moveTo(x0, height)
                    for (let i = 0; i < n; ++i) ctx.lineTo(x0 + i * step, yOf(plot.samples[i]))
                    ctx.lineTo(x0 + (n - 1) * step, height)
                    ctx.closePath()
                    ctx.fillStyle = Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.14)
                    ctx.fill()
                    ctx.beginPath()
                    for (let i = 0; i < n; ++i) {
                        const x = x0 + i * step, y = yOf(plot.samples[i])
                        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                    }
                    ctx.lineWidth = 2
                    ctx.lineJoin = "round"
                    ctx.strokeStyle = Theme.accent
                    ctx.stroke()
                }
            }

            // Hover: crosshair + value.
            MouseArea {
                anchors.fill: chart
                hoverEnabled: true
                onExited: plot.hover = -1
                onPositionChanged: (m) => {
                    const n = plot.samples.length
                    const step = width / 119
                    const i = Math.round((m.x - (width - (n - 1) * step)) / step)
                    plot.hover = i >= 0 && i < n ? i : -1
                }
            }
            Rectangle {
                visible: plot.hover >= 0
                x: chart.x + chart.width - (plot.samples.length - 1 - plot.hover) * (chart.width / 119)
                y: chart.y
                width: 1
                height: chart.height
                color: Theme.textDim
            }
            Rectangle {
                visible: plot.hover >= 0
                readonly property real cx: chart.x + chart.width - (plot.samples.length - 1 - plot.hover) * (chart.width / 119)
                x: Math.min(Math.max(0, cx - width / 2), plot.width - width)
                y: chart.y - 2
                width: tip.implicitWidth + 12
                height: tip.implicitHeight + 6
                radius: 4
                color: Theme.text
                Text {
                    id: tip
                    anchors.centerIn: parent
                    text: plot.hover >= 0 ? App.fmtSpeed(plot.samples[plot.hover]) + "  ·  "
                                            + qsTr("%1 s ago").arg(plot.samples.length - 1 - plot.hover) : ""
                    color: Theme.surface
                    font.pixelSize: 11
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.topMargin: 4; height: 1; color: Theme.border }

        // Totals: plain numbers, not a chart.
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Repeater {
                model: [
                    { label: qsTr("Today"), down: pop.usage.todayDown, up: pop.usage.todayUp },
                    { label: qsTr("This month"), down: pop.usage.monthDown, up: pop.usage.monthUp },
                    { label: qsTr("All time"), down: pop.usage.totalDown, up: pop.usage.totalUp }
                ]
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1 // equal columns
                    spacing: 2
                    Text { text: modelData.label; color: Theme.textDim; font.pixelSize: Theme.fontSmall }
                    Text {
                        text: "↓ " + App.fmtSize(modelData.down || 0)
                        color: Theme.text
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                    Text { text: "↑ " + App.fmtSize(modelData.up || 0); color: Theme.textDim; font.pixelSize: Theme.fontSmall }
                }
            }
        }
    }
}
