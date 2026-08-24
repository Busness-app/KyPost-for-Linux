import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import com.kysecurity.mail 1.0

// Compact icon-only button for toolbars/action rows where a full text
// button (PrimaryButton/GhostButton/DangerButton) reads as too heavy -- e.g.
// EmailDetail's action row. Same interchangeable shape as those three
// (enabled/clicked()) with `icon`/`tooltip` in place of `text`, and a
// `variant` ("ghost"|"primary"|"danger") picking which of their three color
// treatments to mirror, so this reads as a sibling of that family rather
// than a fourth, inconsistent button style. Icons come from the system
// Breeze icon theme via Kirigami.Icon (isMask: true recolors the glyph from
// Theme tokens, same as everything else in this app), not a bundled asset
// set -- freedesktop.org standard icon names (e.g. "edit-delete") are safest
// since they resolve under any conformant icon theme, not just Breeze.
Rectangle {
    id: root

    property string icon: ""
    property string tooltip: ""
    property string variant: "ghost" // "ghost" | "primary" | "danger"
    signal clicked()

    readonly property int buttonSize: 36
    readonly property int iconSize: 18

    implicitWidth: buttonSize
    implicitHeight: buttonSize
    radius: Theme.shapeButton

    readonly property color restColor: variant === "primary" ? Theme.accent
        : (variant === "danger" ? Theme.dangerFillColor : "transparent")
    readonly property color hoverColor: variant === "primary" ? Theme.accent
        : (variant === "danger" ? Theme.dangerFillColor : Theme.panel)
    readonly property color iconColor: variant === "primary" ? Theme.readableOnAccent
        : (variant === "danger" ? Theme.dangerColor : Theme.inkStrong)

    color: (hoverHandler.hovered && !tapHandler.pressed) ? hoverColor : restColor
    border.width: variant === "primary" ? 0 : 1
    border.color: variant === "danger" ? Theme.dangerBorderColor : Theme.line
    opacity: !root.enabled ? 0.5 : (tapHandler.pressed ? 0.85 : (hoverHandler.hovered ? 0.92 : 1.0))

    Behavior on color {
        ColorAnimation { duration: 120 }
    }
    Behavior on opacity {
        NumberAnimation { duration: 120 }
    }

    Kirigami.Icon {
        anchors.centerIn: parent
        width: root.iconSize
        height: root.iconSize
        source: root.icon
        color: root.iconColor
        isMask: true
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

    ToolTip.visible: root.tooltip !== "" && hoverHandler.hovered
    ToolTip.text: root.tooltip
    ToolTip.delay: 500
}
