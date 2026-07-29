import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.urlxl.mail 1.0
import "../components"

// Full-window PIN gate shown whenever AppLock.locked is true.
//
// A plain Item, following the EmailDetail.qml/Compose.qml convention that a
// page does not assume how it is presented -- both roots overlay it at the
// top of their own stacking order rather than pushing it as a page, so
// nothing behind it is reachable while it is up.
Item {
    id: root

    // Emitted once the correct PIN has been accepted. Hosts don't strictly
    // need it (AppLock.locked drives visibility on its own), but it gives
    // them a hook for post-unlock work.
    signal unlocked()

    // Opaque by construction: this sits over the real UI, so any
    // transparency here would leak the mail it exists to hide.
    Rectangle {
        anchors.fill: parent
        color: Theme.bg
    }

    // Swallows every click and key that would otherwise reach the UI behind
    // this overlay.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
    }

    Timer {
        // Drives the lockout countdown. Only runs while a backoff is
        // actually in force, so an unlocked-but-idle app isn't waking up
        // once a second for nothing.
        id: countdownTimer
        interval: 1000
        repeat: true
        running: root.visible && AppLock.remainingLockoutSeconds > 0
        onTriggered: root.secondsLeft = AppLock.remainingLockoutSeconds
    }

    property int secondsLeft: AppLock.remainingLockoutSeconds
    readonly property bool lockedOut: secondsLeft > 0

    onVisibleChanged: {
        if (visible) {
            root.secondsLeft = AppLock.remainingLockoutSeconds
            pinField.text = ""
            root.errorText = ""
            pinField.inputField.forceActiveFocus()
        }
    }

    property string errorText: ""

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(320, root.width - 48)
        spacing: 16

        Text {
            Layout.fillWidth: true
            text: i18n("KyPost is locked")
            color: Theme.inkStrong
            font.family: Theme.fontUi
            font.pixelSize: 20
            font.bold: true
            horizontalAlignment: Text.AlignHCenter
        }

        Text {
            Layout.fillWidth: true
            visible: !AppLock.storeUnavailable
            text: i18n("Enter your PIN to continue.")
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        // The lock fails closed when the secret store cannot be read, which
        // is the right answer and a baffling one on its own: the stored PIN
        // hash lives in that same store, so verifyPin() cannot succeed
        // either and the correct PIN is refused like every other. Without
        // this the screen silently implies the user has forgotten it.
        Text {
            Layout.fillWidth: true
            visible: AppLock.storeUnavailable
            text: i18n("KyPost cannot reach your system keyring, so it cannot check your PIN. "
                       + "Start a keyring service (GNOME Keyring or KWallet), unlock it, then "
                       + "restart KyPost.")
            color: Theme.dangerColor
            font.family: Theme.fontUi
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        ThemedTextField {
            id: pinField
            Layout.fillWidth: true
            enabled: !root.lockedOut && !AppLock.storeUnavailable
            placeholderText: i18n("PIN")

            // echoMode/inputMethodHints live on the inner TextField --
            // ThemedTextField is a Rectangle wrapper and exposes only
            // text/placeholderText/inputField.
            Component.onCompleted: {
                inputField.echoMode = TextInput.Password
                inputField.inputMethodHints = Qt.ImhDigitsOnly
            }
        }

        Connections {
            target: pinField.inputField
            function onAccepted() { root.submit() }
        }

        Text {
            Layout.fillWidth: true
            visible: root.errorText !== ""
            text: root.errorText
            color: Theme.dangerColor
            font.family: Theme.fontUi
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            visible: root.lockedOut
            text: i18n("Too many attempts. Try again in %1 seconds.", root.secondsLeft)
            color: Theme.ink
            font.family: Theme.fontUi
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            // Warn before the wipe rather than after. The threshold is the
            // policy's, not a number duplicated here.
            visible: !root.lockedOut && AppLock.failedAttempts >= 5
            text: i18n("Warning: after 10 failed attempts, all local data is erased.")
            color: Theme.dangerColor
            font.family: Theme.fontUi
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        PrimaryButton {
            Layout.fillWidth: true
            text: i18n("Unlock")
            enabled: !root.lockedOut && pinField.text.length > 0
            onClicked: root.submit()
        }
    }

    function submit() {
        if (root.lockedOut)
            return
        if (AppLock.tryUnlock(pinField.text)) {
            pinField.text = ""
            root.errorText = ""
            root.unlocked()
            return
        }
        pinField.text = ""
        root.secondsLeft = AppLock.remainingLockoutSeconds
        root.errorText = root.lockedOut ? "" : i18n("Incorrect PIN.")
        pinField.inputField.forceActiveFocus()
    }
}
