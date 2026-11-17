pragma Singleton
import QtQuick

QtObject {
    readonly property bool dark: App.theme === "dark"
                                 || (App.theme === "system" && Qt.styleHints.colorScheme === Qt.ColorScheme.Dark)

    readonly property color bg:           dark ? "#1b1d21" : "#f6f7f9"
    readonly property color surface:      dark ? "#23262b" : "#ffffff"
    readonly property color surfaceAlt:   dark ? "#2b2f35" : "#eef0f3"
    readonly property color border:       dark ? "#363a41" : "#e1e4e8"
    readonly property color text:         dark ? "#e6e8eb" : "#1f2328"
    readonly property color textDim:      dark ? "#9aa1ab" : "#656d76"
    readonly property color textDisabled: dark ? "#5c636d" : "#b1b7bf"
    readonly property color accent:       dark ? "#4c8dff" : "#2f7cf6"
    readonly property color accentHover:  dark ? "#6aa0ff" : "#1f6be0"
    readonly property color hover:        dark ? "#2c3037" : "#eef2f7"
    readonly property color pressed:      dark ? "#353a42" : "#e2e8f0"
    readonly property color selection:    dark ? "#233756" : "#dde9fd"
    readonly property color track:        dark ? "#363a41" : "#e4e7eb"
    readonly property color success:      dark ? "#3fb950" : "#2da44e"
    readonly property color error:        dark ? "#f85149" : "#d1242f"
    readonly property color warning:      dark ? "#d29922" : "#bf8700"

    // Windows 11 ships "Segoe Fluent Icons", Windows 10 "Segoe MDL2 Assets" (same code points).
    readonly property string iconFont: Qt.fontFamilies().indexOf("Segoe Fluent Icons") >= 0
                                       ? "Segoe Fluent Icons" : "Segoe MDL2 Assets"

    readonly property var icons: ({
        add: "", play: "", pause: "", del: "", search: "",
        menu: "", folder: "", open: "", settings: "", refresh: "",
        close: "", down: "", up: "", copy: "", link: "",
        chevronDown: "", chevronUp: "", check: "", error: "",
        clock: "", bolt: "", info: ""
    })

    function categoryGlyph(cat) {
        switch (cat) {
        case "Video": return ""
        case "Music": return ""
        case "Archive": return ""
        case "Docs": return ""
        case "Programs": return ""
        case "Pictures": return ""
        default: return ""
        }
    }

    function categoryColor(cat) {
        switch (cat) {
        case "Video": return "#e5534b"
        case "Music": return "#a371f7"
        case "Archive": return "#d29922"
        case "Docs": return "#2f7cf6"
        case "Programs": return "#2da44e"
        case "Pictures": return "#db61a2"
        default: return "#8b949e"
        }
    }
}
