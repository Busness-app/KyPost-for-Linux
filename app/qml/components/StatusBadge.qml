import QtQuick 2.15
import com.kysecurity.mail 1.0

// STYLE_GUIDE.md §4 "Status badge + dot" (mirrors web's `.users-badge`,
// `.contacts-status-active/inactive`): pill-outline badge (stadium shape,
// radius = height / 2, same convention as PillTab) with a small leading
// circular dot. Active uses the theme-invariant success semantic colors --
// Theme.successBorderColor for the outline + dot, Theme.successTextColor
// for the label, per STYLE_GUIDE's two-tone split (dot mirrors the outline
// color, text is its own tone). Inactive deliberately does NOT use
// Theme.dangerColor or any other semantic-color constant -- STYLE_GUIDE §1
// is explicit that "inactive status uses line/panel/ink from the active
// palette, not a fixed color" -- so inactive reads Theme.line (outline +
// dot, mirroring the active outline/dot pairing), Theme.panel (fill), and
// Theme.ink (text), all of which live-update on theme change.
Rectangle {
    id: root

    // Public API.
    property bool active: false
    property string text: ""

    // Padding/dot-size/font-size values below aren't specified by
    // STYLE_GUIDE.md (it only pins colors/radii, which come from Theme);
    // picked as reasonable defaults for a compact status chip.
    readonly property int horizontalPadding: 10
    readonly property int verticalPadding: 5
    readonly property int dotDiameter: 7
    readonly property int dotSpacing: 6

    implicitWidth: rowContent.implicitWidth + horizontalPadding * 2
    implicitHeight: rowContent.implicitHeight + verticalPadding * 2

    radius: height / 2
    color: root.active ? "transparent" : Theme.panel
    border.width: 1
    border.color: root.active ? Theme.successBorderColor : Theme.line

    Behavior on color {
        ColorAnimation { duration: 120 }
    }
    Behavior on border.color {
        ColorAnimation { duration: 120 }
    }

    Row {
        id: rowContent
        anchors.centerIn: parent
        spacing: root.dotSpacing

        Rectangle {
            width: root.dotDiameter
            height: root.dotDiameter
            radius: width / 2
            anchors.verticalCenter: parent.verticalCenter
            color: root.active ? Theme.successBorderColor : Theme.line

            Behavior on color {
                ColorAnimation { duration: 120 }
            }
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: root.active ? Theme.successTextColor : Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 12
            font.weight: Font.Medium

            Behavior on color {
                ColorAnimation { duration: 120 }
            }
        }
    }
}
