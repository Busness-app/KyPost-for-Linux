import QtQuick 2.15
import QtQuick.Layouts 1.15
import com.urlxl.mail 1.0

// The one place a user is asked to re-anchor this device's TLS trust.
//
// WHY IT SHOWS FINGERPRINTS
//
// Before this existed, a certificate change had exactly one route out:
// "remove this pairing in Settings and pair again". That deregisters the
// device server-side and forces the user to obtain a fresh pairing link, to
// recover from what is usually an ordinary certificate renewal. It is also
// not the safer choice it looks like -- re-pairing trusts on first use
// against whatever answers, exactly as reconnecting does. The pain bought a
// weak out-of-band check (you must be signed in somewhere to mint a link)
// and cost a server-side deregistration.
//
// So the affirmative action here is a genuine trust decision, and a
// confirmation that cannot name what is being trusted is one nobody can
// reason about. Both fingerprints are shown, in the form
// `openssl pkey -pubin -outform der | sha256sum` prints, so the user can
// compare against their own server rather than guess. An ordinary renewal
// keeps the key (and this dialog never appears); a renewal that rotates the
// key is verifiable by the admin; an interception is not.
//
// NOT A POPUP, deliberately. Same overlay-Item shape as
// PickupFallbackDialog.qml: a QQC2 Popup renders inside QQuickOverlay, which
// Qt stacks ABOVE ordinary siblings including the app-lock overlay at
// z: 1000 -- a dialog left open when the app locks would stay visible and
// clickable over the PIN screen (AGENTS.md 6d). This is an Item under that z,
// so the lock covers it. PairingController::reconnectToServer() refuses while
// locked as well; the C++ guard is the one that counts, this is the second
// layer.
Item {
    id: root

    property bool isOpen: false

    signal reconnectRequested()

    function open() {
        root.isOpen = true
        panel.forceActiveFocus()
    }

    function close() {
        root.isOpen = false
    }

    function cancel() {
        root.close()
    }

    // Closes before emitting: reconnect kicks off a registration and the
    // dialog has no further part in it.
    function confirm() {
        root.close()
        root.reconnectRequested()
    }

    // Never outlive a lock. Belt and braces with the C++ guard: this dialog's
    // affirmative action rotates the device credential.
    Connections {
        target: AppLock
        function onLockedChanged() {
            if (AppLock.locked)
                root.close()
        }
    }

    visible: root.isOpen
    anchors.fill: parent
    z: 960

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.4)

        TapHandler {
            onTapped: root.cancel()
        }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(parent ? parent.width - 32 : 460, 460)
        implicitHeight: content.implicitHeight + 32
        radius: Theme.shapeSheet
        color: Theme.panel
        border.width: 1
        border.color: Theme.line

        // Escape cancels, like the scrim and the Cancel button: the safe path
        // is the one a reflex keystroke reaches. No button takes focus, so no
        // keystroke can reach the affirmative action.
        Keys.onEscapePressed: function (event) {
            root.cancel()
            event.accepted = true
        }

        TapHandler {} // swallow taps so they don't reach the scrim behind

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Text {
                text: i18n("This server's security certificate changed")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 16
                font.weight: Font.Medium
                textFormat: Text.PlainText
            }

            Text {
                Layout.fillWidth: true
                text: i18n("KyPost pinned this server's key when this device was paired, and the key it is presenting now is different. Every request is being refused until you decide.")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 14
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
            }

            SectionLabel { text: i18n("Pinned when paired") }
            Text {
                Layout.fillWidth: true
                text: Pairing.expectedCertificateFingerprint
                color: Theme.ink
                font.family: Theme.fontMono
                font.pixelSize: 11
                wrapMode: Text.WrapAnywhere
                // Never AutoText on a value like this: a markup-shaped string
                // would be handed to the StyledText parser, which fetches
                // <img src> over the QML engine's own network stack
                // (AGENTS.md 6d).
                textFormat: Text.PlainText
            }

            SectionLabel { text: i18n("Presented now") }
            Text {
                Layout.fillWidth: true
                text: Pairing.observedCertificateFingerprint.length > 0
                      ? Pairing.observedCertificateFingerprint
                      : i18n("This server's key could not be read at all.")
                color: Theme.dangerColor
                font.family: Theme.fontMono
                font.pixelSize: 11
                wrapMode: Text.WrapAnywhere
                textFormat: Text.PlainText
            }

            Text {
                Layout.fillWidth: true
                // States the consequence, not a reassurance. Reconnecting
                // sends this device's credential to whoever holds that key.
                text: i18n("Only continue if you know why it changed — ask whoever runs this server to confirm the fingerprint above. Reconnecting sends this device's credentials to whoever holds that key.")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 13
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                GhostButton {
                    text: i18n("Cancel")
                    onClicked: root.cancel()
                }
                DangerButton {
                    text: i18n("Trust and reconnect")
                    onClicked: root.confirm()
                }
            }
        }
    }
}
