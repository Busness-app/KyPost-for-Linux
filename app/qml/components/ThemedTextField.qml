import QtQuick 2.15
import QtQuick.Controls 2.15
import com.kysecurity.mail 1.0

// New component (Task 35): none of the other 7 existing app/qml/components/
// *.qml delegates cover a themed single-line text entry, and Compose.qml
// needs the same styled field four times over (To/Cc/Bcc/Subject) --
// promoted here per Phase 6 global constraint 9 rather than duplicated
// inline. Theme.shapeField is exactly the radius ThemeController already
// carves out for this shape (see ThemeController.h), so this is filling a
// gap the theme anticipated, not inventing a new one.
//
// Judgment call: wraps QtQuick.Controls' TextField rather than a bare
// TextInput -- TextField already supplies keyboard/IME/selection/clipboard
// handling for free; only `background`/`color`/`font`/padding are re-themed
// here (background set to null so this Rectangle's own fill/border show
// through instead of the platform style's default field chrome).
Rectangle {
    id: root

    // Public API -- aliases straight onto the inner TextField so callers
    // can bind/read `text`/`placeholderText` exactly as they would on a
    // plain TextField.
    property alias text: field.text
    property alias placeholderText: field.placeholderText
    readonly property alias inputField: field

    readonly property int verticalPadding: 10
    readonly property int horizontalPadding: 12

    implicitWidth: 260
    implicitHeight: field.implicitHeight + verticalPadding * 2

    radius: Theme.shapeField
    color: Theme.panel
    border.width: 1
    border.color: field.activeFocus ? Theme.accent : Theme.line

    Behavior on border.color {
        ColorAnimation { duration: 120 }
    }

    TextField {
        id: field
        anchors.fill: parent
        leftPadding: root.horizontalPadding
        rightPadding: root.horizontalPadding
        topPadding: root.verticalPadding
        bottomPadding: root.verticalPadding
        background: null
        color: Theme.inkStrong
        placeholderTextColor: Theme.ink
        font.family: Theme.fontUi
        font.pixelSize: 14
        selectByMouse: true
    }
}
