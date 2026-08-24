import QtQuick 2.15
import com.kysecurity.mail 1.0

// Compose autocomplete (ContactAutocomplete.md): a brief self-clearing
// banner for the duplicate-selection-rejected case. Mirrors ContactsList.qml's
// existing syncStatusTimer "show, then hide after N seconds" pattern (the
// one precedent for this shape already in the codebase) rather than
// inventing a new transient-message mechanism.
Rectangle {
    id: root

    property string text: ""

    readonly property int horizontalPadding: 14
    readonly property int verticalPadding: 8

    function show(message) {
        root.text = message
        root.visible = true
        hideTimer.restart()
    }

    visible: false
    implicitWidth: label.implicitWidth + horizontalPadding * 2
    implicitHeight: label.implicitHeight + verticalPadding * 2
    radius: Theme.shapeButton
    color: Theme.panel
    border.width: 1
    border.color: Theme.line

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
        font.pixelSize: 12
    }

    Timer {
        id: hideTimer
        interval: 2500
        onTriggered: root.visible = false
    }
}
