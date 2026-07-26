import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import com.urlxl.mail 1.0
import "../components"

// Task 37 -- plain reusable Item, deliberately NOT a Kirigami.Page (see
// Phase 6 global constraint 4): MobileRoot wraps this in a thin
// Kirigami.Page shell when it pushes it (Task 38); DesktopRoot hosts it as
// a modal/overlay (Task 39). State-driven off Pairing.pairingState -- this
// file never calls pairFromParsedParams/the network directly, it only
// forwards user input to the Pairing singleton (Task 34) and renders
// whatever state that singleton reports.
//
// Paste-link only, no camera/QR scanning -- global constraint 6: desktop
// pairing is explicitly out of scope, and neither reference client's QR
// path has a Qt/QML equivalent available in this phase. Deep-link arrival
// (kypost://native-pair) is routed straight to
// Pairing.pairFromDeepLink() from main.cpp's routeDeepLink(), independent
// of whether this page happens to be on screen -- this file only needs to
// reflect pairingState once that call lands, which the state machine below
// already does regardless of which entry point (paste or deep link)
// triggered it.
Item {
    id: root

    // Emitted from the "paired" state's Done button -- same "don't assume
    // push-navigation vs. a pane" shape as EmailDetail/Compose/ContactDetail
    // (Tasks 35/36): whichever root hosts this decides what "close" means.
    signal closed()

    implicitWidth: 360
    implicitHeight: 640

    // Never log the pasted text itself -- it carries the same sub/hash/
    // pairing-token credential material PairingController's own doc
    // comment already treats as sensitive (see PairingController.h). This
    // file must not add a qDebug()/console.log of `pasteField.text`
    // anywhere.

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 16

        Item { Layout.fillHeight: true; Layout.preferredHeight: 1 }

        // ---- idle --------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: Pairing.pairingState === "idle"
            spacing: 16

            Text {
                Layout.fillWidth: true
                text: "✦" // sparkle glyph -- no image asset exists in this repo
                color: Theme.accent
                font.pixelSize: 40
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Pair Device")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 22
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Open a pairing link from the web app, or paste it below.")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            ThemedTextField {
                id: pasteField
                Layout.fillWidth: true
                // Not wrapped in i18n() -- this is an example of the
                // kypost:// URL scheme itself (a technical wire format,
                // not natural-language prose), same reasoning as leaving
                // StandardFolder wire names untranslated.
                placeholderText: "kypost://native-pair?…"
                inputField.font.family: Theme.fontMono
            }

            PrimaryButton {
                Layout.alignment: Qt.AlignHCenter
                text: i18n("Pair")
                enabled: pasteField.text.trim() !== ""
                onClicked: Pairing.pairFromPastedLink(pasteField.text)
            }
        }

        // ---- confirm ---------------------------------------------------
        // VibeSec fix: a kypost://native-pair link (deep link or
        // pasted) no longer pairs immediately on arrival -- see
        // PairingController::pairFromDeepLink's doc comment. This step
        // shows which server is asking before Pairing.confirmPendingPair()
        // makes any network call, so a link clicked outside this app (this
        // app is the OS-wide handler for the kypost:// scheme) can't
        // silently re-pair the device.
        ColumnLayout {
            Layout.fillWidth: true
            visible: Pairing.pairingState === "confirm"
            spacing: 16

            Text {
                Layout.fillWidth: true
                text: "⚠" // warning glyph -- this is a security-sensitive confirmation, not routine chrome
                color: Theme.accent
                font.pixelSize: 40
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Pair with this server?")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 22
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: i18n("A pairing request wants to connect this device to:")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: Pairing.pendingPairOrigin
                color: Theme.inkStrong
                font.family: Theme.fontMono
                font.pixelSize: 16
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            // Only reachable for loopback, but cleartext must never look the
            // same as TLS in the one confirmation the user gets -- the pairing
            // token and the real push device token both travel over this.
            Text {
                Layout.fillWidth: true
                visible: Pairing.pendingPairInsecure
                text: i18n("This connection is not encrypted (http).")
                color: Theme.dangerColor
                font.family: Theme.fontUi
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Only confirm this if you expected it. Once paired, this server can deliver mail notifications to this device.")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 12
                GhostButton {
                    text: i18n("Cancel")
                    onClicked: Pairing.cancelPendingPair()
                }
                PrimaryButton {
                    text: i18n("Pair")
                    onClicked: Pairing.confirmPendingPair()
                }
            }
        }

        // ---- working -------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: Pairing.pairingState === "working"
            spacing: 16

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: Pairing.pairingState === "working"
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Pairing…")
                color: Theme.ink
                font.family: Theme.fontUi
                font.pixelSize: 15
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // ---- paired ----------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: Pairing.pairingState === "paired"
            spacing: 16

            Text {
                Layout.fillWidth: true
                text: "✓" // check mark -- success glyph, no image asset needed
                color: Theme.successBorderColor
                font.pixelSize: 40
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Device paired")
                color: Theme.inkStrong
                font.family: Theme.fontUi
                font.pixelSize: 22
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    SectionLabel { Layout.preferredWidth: 70; text: i18n("Server") }
                    Text {
                        Layout.fillWidth: true
                        text: Pairing.pairedServerHost
                        color: Theme.inkStrong
                        font.family: Theme.fontMono
                        font.pixelSize: 14
                        wrapMode: Text.WordWrap
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    SectionLabel { Layout.preferredWidth: 70; text: i18n("Device") }
                    Text {
                        Layout.fillWidth: true
                        text: Pairing.deviceId
                        color: Theme.inkStrong
                        font.family: Theme.fontMono
                        font.pixelSize: 14
                        wrapMode: Text.WordWrap
                    }
                }
            }
            PrimaryButton {
                Layout.alignment: Qt.AlignHCenter
                text: i18n("Done")
                onClicked: root.closed()
            }
        }

        // ---- failed ----------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: Pairing.pairingState === "failed"
            spacing: 16

            Text {
                Layout.fillWidth: true
                text: "⚠" // warning glyph
                color: Theme.dangerColor
                font.pixelSize: 40
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                text: i18n("Pairing failed")
                color: Theme.dangerColor
                font.family: Theme.fontUi
                font.pixelSize: 22
                font.weight: Font.Bold
                horizontalAlignment: Text.AlignHCenter
            }
            Text {
                Layout.fillWidth: true
                // Pairing.pairingError is set by PairingController from a
                // fixed set of i18n()-wrapped chrome messages (see
                // PairingController.cpp) or, on RegistrationOutcome::Failure,
                // from the relay's own free-text detail -- either way the
                // string arriving here is already final display text, not a
                // literal to wrap a second time.
                text: Pairing.pairingError
                color: Theme.dangerColor
                font.family: Theme.fontUi
                font.pixelSize: 14
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }
            GhostButton {
                Layout.alignment: Qt.AlignHCenter
                text: i18n("Try Again")
                onClicked: Pairing.reset()
            }
        }

        Item { Layout.fillHeight: true; Layout.preferredHeight: 1 }
    }
}
