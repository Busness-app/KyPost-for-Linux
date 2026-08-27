import QtQuick 2.15
import com.kysecurity.mail 1.0

// STYLE_GUIDE.md §4 "Ghost/secondary button" (mirrors web's
// `.notifications-ghost`): transparent fill, 1px Theme.line stroke,
// Theme.inkStrong text, same Theme.shapeButton radius as PrimaryButton.
// Same `text`/`enabled`/`clicked()` API shape as PrimaryButton/DangerButton
// for interchangeability.
Rectangle {
    id: root

    // Public API. `enabled` is Item's own built-in property (defaults to
    // true) -- reused as-is rather than re-declared, since QML forbids
    // shadowing a property already defined on the base type.
    property string text: ""
    signal clicked()

    // Padding/font-size values below aren't specified by STYLE_GUIDE.md (it
    // only pins colors/radii, which come from Theme); picked to match
    // PrimaryButton so the two are visually interchangeable in a layout.
    readonly property int horizontalPadding: 20
    readonly property int verticalPadding: 12

    implicitWidth: label.implicitWidth + horizontalPadding * 2
    implicitHeight: label.implicitHeight + verticalPadding * 2

    radius: Theme.shapeButton
    color: hoverHandler.hovered && !tapHandler.pressed ? Theme.panel : "transparent"
    border.width: 1
    border.color: Theme.line
    opacity: !root.enabled ? 0.5 : (tapHandler.pressed ? 0.85 : 1.0)

    Behavior on color {
        ColorAnimation { duration: 120 }
    }
    Behavior on opacity {
        NumberAnimation { duration: 120 }
    }

    Text {
        id: label
        anchors.centerIn: parent
        textFormat: Text.PlainText
        text: root.text
        color: Theme.inkStrong
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
