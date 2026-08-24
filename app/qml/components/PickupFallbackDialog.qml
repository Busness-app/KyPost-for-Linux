import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.kysecurity.mail 1.0

// Confirmation for the one-time-link fallback, shown when the relay refuses an
// encrypted send with 409 + keylessRecipients.
//
// The wording here is contract, not copy to be improved: it is the entire
// reason the opt-in exists. The recipients are named explicitly rather than
// summarized as "some recipients", and the seven-day server-side plaintext
// retention is stated plainly. Every pickup link this client can cause is the
// server-readable kind -- the browser's sealed variant is gated to
// client-custody accounts, which cannot send from this app at all -- so
// softening this text would misrepresent what the user is agreeing to.
//
// Same overlay-Item shape as HyperlinkDialog.qml / AddressBookPickerDialog.qml
// (there is no QtQuick.Controls.Dialog precedent in this codebase). Cancel is
// the safe path and is reachable four ways: the Cancel button, tapping the
// scrim, Escape, and simply not confirming -- the affirmative action is the
// only one that requires a deliberate press on a distinct button. No button
// takes focus, so no keystroke can trigger the affirmative action either;
// that asymmetry is deliberate, not an oversight of the overlay-Item shape.
Item {
    id: root

    property bool isOpen: false
    // The server's own list of addresses with no usable key, as delivered by
    // MailController::pickupFallbackRequired.
    property var recipients: []
    // MailController's identifier for the one pending send this dialog is
    // asking about, handed straight back on confirm so a confirmation cannot
    // resolve some other composition's refusal. `var` rather than a typed
    // property because QML has no native quint64; it crosses the bridge as a
    // JS number, exact for any value this counter can reach.
    property var token: 0

    signal confirmed(var token)
    signal cancelled()

    function open(pendingSendToken, addresses) {
        root.token = pendingSendToken
        root.recipients = addresses
        root.isOpen = true
        // Escape below only works with active focus, and this overlay is
        // created (not pushed onto a focus-managing stack), so claim it here.
        // Not handed back on close: the composer's previously-focused field is
        // not recorded, and the one field that usually holds focus is a
        // WebEngineView whose caret does not reliably survive a
        // forceActiveFocus() round trip. Cancelling therefore leaves keyboard
        // focus unset until the user clicks a field -- a deliberate trade for
        // making Escape work at all.
        panel.forceActiveFocus()
    }

    function close() {
        root.isOpen = false
        root.recipients = []
        root.token = 0
    }

    // Cancelling drops the composition's cached plaintext rather than holding
    // it for a confirm that may never come.
    function cancel() {
        root.cancelled()
        root.close()
    }

    // Closes BEFORE emitting, and carries the token in the signal rather than
    // leaving the handler to read it back off this dialog: confirming kicks
    // off a synchronous send, so the dialog must already be gone, and close()
    // resets the property.
    function confirm() {
        const confirmedToken = root.token
        root.close()
        root.confirmed(confirmedToken)
    }

    visible: root.isOpen
    anchors.fill: parent

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
        width: 360
        implicitHeight: content.implicitHeight + 32
        radius: Theme.shapeSheet
        color: Theme.panel
        border.width: 1
        border.color: Theme.line

        // Escape cancels, the same as the Cancel button and the scrim: the
        // safe path is the one a reflex keystroke can reach.
        Keys.onEscapePressed: function (event) {
            root.cancel()
            event.accepted = true
        }

        TapHandler {} // swallow taps -- keeps them from reaching the scrim behind

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            Text {
                text: i18n("Send an unencrypted link?")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 16
                font.weight: Font.Medium
            }
            Text {
                Layout.fillWidth: true
                text: i18np("We don't have a PGP key for %2. They'll get an email with a one-time link instead.",
                            "We don't have PGP keys for %2. They'll get an email with a one-time link instead.",
                            root.recipients.length, root.recipients.join(", "))
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 14
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                // The seven days and the word "unencrypted" are the security
                // property this dialog exists to disclose. Do not soften.
                text: i18n("To make that work, this message's contents are stored on your KyPost server — unencrypted — for up to 7 days or until the link is opened. Everyone else on this message still gets it encrypted.")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Item { Layout.fillWidth: true }
                GhostButton {
                    text: i18n("Cancel")
                    onClicked: root.cancel()
                }
                PrimaryButton {
                    text: i18n("Send link anyway")
                    onClicked: root.confirm()
                }
            }
        }
    }
}
