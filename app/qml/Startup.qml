import QtQuick 2.15
import QtQuick.Window 2.15
import com.kysecurity.mail 1.0

// The window KyPost puts up while main() is still building its object graph.
//
// WHY THIS EXISTS. Opening the platform secret store can take ~25 seconds when
// the Secret Service is reachable but wedged -- a locked collection with no
// prompter is enough -- and main() has to consult it before the database,
// because the database's encryption key lives in it. For that whole time the
// application used to have no window at all: not a frozen one, none, which
// reads as a launch that silently failed rather than one that is waiting.
//
// It is deliberately not a splash screen. It appears only when startup is
// actually slow (main() holds it hidden for the first moment), it says what is
// happening rather than showing a logo, and it is closed the instant the real
// root window is ready.
//
// Nothing here may reach ContactsApp, Mail, Pairing or AppLock: none of them
// exist yet. Theme is the one singleton registered before this loads.
Window {
    id: root

    // main() decides when, and whether, this is ever shown.
    visible: false
    width: 460
    height: 220
    minimumWidth: width
    minimumHeight: height
    title: qsTr("KyPost")
    color: Theme.bg

    // What main() is currently waiting on, set from C++.
    property string statusText: qsTr("Starting…")

    Column {
        anchors.centerIn: parent
        spacing: 14

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("KyPost")
            color: Theme.inkStrong
            font.family: Theme.fontUi
            font.pixelSize: 26
            font.weight: Font.DemiBold
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            width: root.width - 64
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: root.statusText
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 14
        }

        // A plain indeterminate bar. QtQuick.Controls is deliberately not
        // imported: this window has to be able to load before anything else in
        // the application does, so it depends on as little as possible.
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 220
            height: 4
            radius: 2
            color: Theme.line

            Rectangle {
                id: pip
                width: 70
                height: parent.height
                radius: parent.radius
                color: Theme.accent

                SequentialAnimation on x {
                    loops: Animation.Infinite
                    running: root.visible
                    NumberAnimation { from: 0; to: 150; duration: 900; easing.type: Easing.InOutQuad }
                    NumberAnimation { from: 150; to: 0; duration: 900; easing.type: Easing.InOutQuad }
                }
            }
        }
    }
}
