import QtQuick 2.15
import QtTest 1.15
import com.kysecurity.mail 1.0
import "qrc:/qml/components" as Components

// The dialog that re-anchors this device's TLS trust is a security control,
// and AGENTS.md 6b requires QML security controls to have a QML test -- the
// rule exists because a total app-lock bypass shipped while 73 C++ tests
// could not see a single .qml file.
//
// What matters here is the asymmetry: the affirmative action must be
// reachable ONLY by a deliberate press, and every other exit -- Cancel, the
// scrim, Escape, the app locking -- must close without reconnecting.
TestCase {
    id: testCase
    name: "CertificateTrustDialog"
    when: windowShown
    visible: true
    width: 600
    height: 400

    Component {
        id: dialogComponent
        Components.CertificateChangeDialog {}
    }

    SignalSpy {
        id: reconnectSpy
        signalName: "reconnectRequested"
    }

    function createDialog() {
        const dialog = createTemporaryObject(dialogComponent, testCase)
        verify(dialog !== null, "dialog must instantiate")
        // Spying on the dialog's own signal is the right seam: turning it
        // into an actual re-registration is the root's wiring
        // (onReconnectRequested: Pairing.reconnectToServer()), not this
        // component's responsibility.
        reconnectSpy.target = dialog
        reconnectSpy.clear()
        return dialog
    }

    function test_showsBothFingerprints() {
        const dialog = createDialog()
        dialog.open()
        verify(dialog.isOpen)
        // Both keys must be on screen: a confirmation that cannot name what
        // is being trusted is one nobody can reason about.
        verify(findText(dialog, Pairing.expectedCertificateFingerprint),
               "the pinned fingerprint must be shown")
        verify(findText(dialog, Pairing.observedCertificateFingerprint),
               "the presented fingerprint must be shown")
    }

    function test_cancelDoesNotReconnect() {
        const dialog = createDialog()
        dialog.open()
        dialog.cancel()
        verify(!dialog.isOpen)
        compare(reconnectSpy.count, 0, "cancelling must never re-anchor trust")
    }

    function test_confirmReconnectsExactlyOnce() {
        const dialog = createDialog()
        dialog.open()
        dialog.confirm()
        verify(!dialog.isOpen, "the dialog must close before the registration starts")
        compare(reconnectSpy.count, 1)
    }

    // A dialog left open when the app locks must not stay actionable. The C++
    // guard in reconnectToServer() refuses while locked and is the one that
    // counts; this is the second layer, and the one that keeps account state
    // off the screen above the PIN prompt.
    function test_lockingClosesTheDialogWithoutReconnecting() {
        const dialog = createDialog()
        dialog.open()
        verify(dialog.isOpen)

        AppLock.locked = true
        verify(!dialog.isOpen, "locking must close the certificate dialog")
        compare(reconnectSpy.count, 0)
        AppLock.locked = false
    }

    // Recursively looks for a Text whose text contains `needle`.
    function findText(item, needle) {
        if (item === null || needle.length === 0)
            return false
        if (item.text !== undefined && String(item.text).indexOf(needle) !== -1)
            return true
        for (let i = 0; i < item.children.length; ++i) {
            if (findText(item.children[i], needle))
                return true
        }
        return false
    }
}
