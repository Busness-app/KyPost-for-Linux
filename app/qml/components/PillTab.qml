import QtQuick 2.15
import com.urlxl.mail 1.0

// STYLE_GUIDE.md §4 "Pill filter tabs / segmented toggle" (mirrors web's
// `.inbox-page-tab`, `.contacts-page-tab`, `.notifications-delivery-toggle`)
// and §3's "full stadium" shape row: radius = height / 2, deliberately NOT
// one of the Theme.shape* constants (those are for rectangular
// panels/buttons/fields, never pills/tabs/toggles per STYLE_GUIDE.md §3).
// Inactive = transparent fill + 1px Theme.line stroke + Theme.ink text.
// Active = Theme.accent fill + Theme.readableOnAccent text, no stroke.
Rectangle {
    id: root

    // Public API. `enabled` is Item's own built-in property (defaults to
    // true) -- reused as-is rather than re-declared, since QML forbids
    // shadowing a property already defined on the base type.
    property string text: ""
    property bool selected: false
    signal clicked()

    // Padding/font-size values below aren't specified by STYLE_GUIDE.md (it
    // only pins colors/radii, which come from Theme); picked as reasonable
    // defaults for a compact pill chip.
    readonly property int horizontalPadding: 16
    readonly property int verticalPadding: 8

    implicitWidth: label.implicitWidth + horizontalPadding * 2
    implicitHeight: label.implicitHeight + verticalPadding * 2

    radius: height / 2
    color: root.selected ? Theme.accent : (hoverHandler.hovered && !tapHandler.pressed ? Theme.panel : "transparent")
    border.width: root.selected ? 0 : 1
    border.color: Theme.line
    opacity: !root.enabled ? 0.5 : (tapHandler.pressed ? 0.85 : (root.selected && hoverHandler.hovered ? 0.92 : 1.0))

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
        color: root.selected ? Theme.readableOnAccent : Theme.ink
        font.family: Theme.fontUi
        font.pixelSize: 13
        font.weight: Font.Medium

        Behavior on color {
            ColorAnimation { duration: 120 }
        }
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
