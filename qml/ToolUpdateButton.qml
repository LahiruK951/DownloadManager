import QtQuick

// "Update to <version>" for FluxDM's own ffmpeg/yt-dlp, shown only when a newer release exists
// ("Download" when there is no copy at all); shows progress while it works.
FButton {
    id: b
    required property var tool
    required property int toolIndex
    visible: tool.busy || tool.canUpdate || tool.found === ""
    enabled: !tool.busy
    kind: "primary"
    text: tool.busy ? tool.status
        : tool.found === "" ? qsTr("Download")
        : qsTr("Update to %1").arg(tool.latest)
    onClicked: App.updateTool(toolIndex)
}
