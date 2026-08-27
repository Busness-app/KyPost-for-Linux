import QtQuick 2.15
import com.kysecurity.mail 1.0

// STYLE_GUIDE.md §4 "Danger button" (mirrors web's `.users-action-danger`,
// `.contacts-action-danger`): 1px Theme.dangerBorderColor stroke +
// Theme.dangerFillColor fill (already composed as 12%-alpha red by
// ThemeController), text in the solid Theme.dangerColor. Same
// `text`/`enabled`/`clicked()` API shape as PrimaryButton/GhostButton for
// interchangeability.
Rectangle {
    id: root

    // Public API. `enabled` is Item's own built-in property (defaults to
    // true) -- reused as-is rather than re-declared, since QML forbids
    // shadowing a property already defined on the base type.
    property string text: ""
    signal clicked()

    // Padding/font-size values below aren't specified by STYLE_GUIDE.md (it
    // only pins colors/radii, which come from Theme); picked to match
    // PrimaryButton/GhostButton so all three are visually interchangeable.
    readonly property int horizontalPadding: 20
    readonly property int verticalPadding: 12

    implicitWidth: label.implicitWidth + horizontalPadding * 2
    implicitHeight: label.implicitHeight + verticalPadding * 2

    radius: Theme.shapeButton
    color: Theme.dangerFillColor
    border.width: 1
    border.color: Theme.dangerBorderColor
    opacity: !root.enabled ? 0.5 : (tapHandler.pressed ? 0.85 : (hoverHandler.hovered ? 0.92 : 1.0))

    Behavior on opacity {
        NumberAnimation { duration: 120 }
    }

    Text {
        id: label
        anchors.centerIn: parent
        textFormat: Text.PlainText
        text: root.text
        color: Theme.dangerColor
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
