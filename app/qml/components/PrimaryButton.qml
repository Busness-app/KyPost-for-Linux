import QtQuick 2.15
import com.kysecurity.mail 1.0

// STYLE_GUIDE.md §4 "Primary button" (mirrors web's `.users-create-submit`):
// solid Theme.accent fill, Theme.shapeButton corner radius, text color
// Theme.readableOnAccent. `text`/`enabled`/`clicked()` API is shared with
// GhostButton/DangerButton so callers can swap between the three
// interchangeably.
Rectangle {
    id: root

    // Public API. `enabled` is Item's own built-in property (defaults to
    // true) -- reused as-is rather than re-declared, since QML forbids
    // shadowing a property already defined on the base type.
    property string text: ""
    signal clicked()

    // Padding/font-size values below aren't specified by STYLE_GUIDE.md (it
    // only pins colors/radii, which come from Theme); picked as reasonable
    // defaults for a touch-friendly button.
    readonly property int horizontalPadding: 20
    readonly property int verticalPadding: 12

    implicitWidth: label.implicitWidth + horizontalPadding * 2
    implicitHeight: label.implicitHeight + verticalPadding * 2

    radius: Theme.shapeButton
    color: Theme.accent
    opacity: !root.enabled ? 0.5 : (tapHandler.pressed ? 0.85 : (hoverHandler.hovered ? 0.92 : 1.0))

    Behavior on opacity {
        NumberAnimation { duration: 120 }
    }

    Text {
        id: label
        anchors.centerIn: parent
        textFormat: Text.PlainText
        text: root.text
        color: Theme.readableOnAccent
        font.family: Theme.fontUi
        font.pixelSize: 15
        font.weight: Font.Medium
    }

    HoverHandler {
        id: hoverHandler
        enabled: root.enabled
    }

    TapHandler {
        id: tapHandler
        enabled: root.enabled
        onTapped: root.clicked()
    }
}
